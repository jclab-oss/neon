//! Local branches, see [`ComputeMode::Local`].
//!
//! A local branch keeps all its changes in its data directory, so unlike other
//! computes, it must not start from a fresh basebackup when the compute restarts.
//!
//! The first time a local branch starts, it starts from a basebackup like a primary.
//! Postgres only starts from a basebackup if the `neon.signal` file is present. So
//! once Postgres is running and has written a checkpoint to its local WAL, we remove
//! that file. From then on, Postgres starts from its own data directory, replaying
//! the local WAL after a crash, like vanilla Postgres.
//!
//! The marker file [`MARKER_FILE`] in the data directory records which branch the
//! data directory belongs to, and whether the first start completed.

use std::path::Path;

use anyhow::{Context, Result, bail};
use camino::Utf8Path;
use compute_api::spec::ComputeMode;
use serde::{Deserialize, Serialize};
use tokio_postgres::NoTls;
use tracing::{info, warn};
use utils::auth::Scope;
use utils::crashsafe;
use utils::id::{TenantId, TimelineId};
use utils::lsn::Lsn;

use crate::compute::{ComputeNode, ParsedSpec};

pub const MARKER_FILE: &str = "neon_local_branch.json";

/// The basebackup's signal files, see `readNeonSignalFile()` in Postgres.
const SIGNAL_FILES: [&str; 2] = ["neon.signal", "zenith.signal"];

#[derive(Serialize, Deserialize, Debug, PartialEq, Eq)]
struct Marker {
    tenant_id: TenantId,
    timeline_id: TimelineId,
    lsn: Lsn,
    /// The first start completed: Postgres no longer starts from the basebackup.
    initialized: bool,
}

impl Marker {
    fn new(spec: &ParsedSpec, lsn: Lsn, initialized: bool) -> Self {
        Self {
            tenant_id: spec.tenant_id,
            timeline_id: spec.timeline_id,
            lsn,
            initialized,
        }
    }

    fn read(pgdata: &Path) -> Result<Option<Self>> {
        let path = pgdata.join(MARKER_FILE);
        match std::fs::read(&path) {
            Ok(content) => Ok(Some(serde_json::from_slice(&content).with_context(
                || format!("could not parse local branch marker {}", path.display()),
            )?)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(e).with_context(|| format!("could not read {}", path.display())),
        }
    }

    fn write(&self, pgdata: &Path) -> Result<()> {
        let pgdata = utf8_path(pgdata)?;
        let path = pgdata.join(MARKER_FILE);
        let tmp_path = pgdata.join(format!("{MARKER_FILE}.tmp"));
        crashsafe::overwrite(&path, &tmp_path, &serde_json::to_vec(self)?)
            .with_context(|| format!("could not write {path}"))
    }
}

fn utf8_path(path: &Path) -> Result<&Utf8Path> {
    Utf8Path::from_path(path).with_context(|| format!("path {} is not UTF-8", path.display()))
}

/// Checks that the spec of a local branch is consistent: it must not use the
/// safekeepers, and the storage token, if any, must be read-only.
pub fn validate_spec(spec: &ParsedSpec) -> Result<(), String> {
    if !spec.safekeeper_connstrings.is_empty() {
        return Err("a local branch must not have safekeepers".to_string());
    }
    if let Some(token) = &spec.storage_auth_token {
        match token_scope(token) {
            Some(Scope::TenantReadOnly) => {}
            scope => {
                return Err(format!(
                    "a local branch requires a tenant_read_only storage token, got scope {scope:?}"
                ));
            }
        }
    }
    Ok(())
}

/// Reads the scope of a JWT, without verifying it. The storage servers verify it;
/// we just want to make sure that we haven't been given more access than we need.
fn token_scope(token: &str) -> Option<Scope> {
    use base64::Engine as _;

    #[derive(Deserialize)]
    struct Claims {
        scope: Scope,
    }

    let payload = token.split('.').nth(1)?;
    let payload = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(payload.trim_end_matches('='))
        .ok()?;
    let claims: Claims = serde_json::from_slice(&payload).ok()?;
    Some(claims.scope)
}

/// Decides whether a local branch can start from its existing data directory.
/// Returns false if it should start from a basebackup.
pub fn can_resume(pgdata: &Path, spec: &ParsedSpec, lsn: Lsn) -> Result<bool> {
    let Some(marker) = Marker::read(pgdata)? else {
        return Ok(false);
    };

    let expected = Marker::new(spec, lsn, marker.initialized);
    if marker != expected {
        bail!(
            "data directory {} belongs to local branch {}/{} at {}, not {}/{} at {}; refusing to overwrite it",
            pgdata.display(),
            marker.tenant_id,
            marker.timeline_id,
            marker.lsn,
            spec.tenant_id,
            spec.timeline_id,
            lsn
        );
    }

    if !marker.initialized {
        // Nothing was acknowledged to clients yet, so it's safe to start over.
        warn!("the first start of the local branch did not complete, starting from a basebackup");
        return Ok(false);
    }

    for signal_file in SIGNAL_FILES {
        if pgdata.join(signal_file).exists() {
            bail!(
                "{signal_file} exists in the data directory of an initialized local branch, refusing to start"
            );
        }
    }

    info!("resuming local branch from its data directory");
    Ok(true)
}

/// Records that the data directory is a new local branch that's being set up from
/// a basebackup.
pub fn init_data_dir(pgdata: &Path, spec: &ParsedSpec, lsn: Lsn) -> Result<()> {
    Marker::new(spec, lsn, false).write(pgdata)
}

/// Called once Postgres runs. If this is the first start of the local branch,
/// makes it start from its own data directory from now on.
pub fn finish_start(compute: &ComputeNode, spec: &ParsedSpec) -> Result<()> {
    let ComputeMode::Local(lsn) = spec.spec.mode else {
        return Ok(());
    };
    let pgdata = Path::new(&compute.params.pgdata);

    match Marker::read(pgdata)? {
        Some(marker) if marker.initialized => return Ok(()),
        Some(_) => {}
        None => bail!("local branch marker is missing"),
    }

    // Restarting without the signal file starts from the latest checkpoint in the
    // local WAL. Make sure there is one.
    let conf = compute.get_tokio_conn_conf(Some("compute_ctl:local_branch"));
    tokio::runtime::Handle::current().block_on(async {
        let (client, conn) = conf.connect(NoTls).await?;
        tokio::spawn(conn);
        client.simple_query("CHECKPOINT").await?;
        anyhow::Ok(())
    })?;

    for signal_file in SIGNAL_FILES {
        std::fs::remove_file(pgdata.join(signal_file))
            .or_else(utils::fs_ext::ignore_not_found)
            .with_context(|| format!("could not remove {signal_file}"))?;
    }
    crashsafe::fsync(utf8_path(pgdata)?)?;

    Marker::new(spec, lsn, true).write(pgdata)?;
    info!("local branch initialized");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_token_scope() {
        // {"scope": "tenant_read_only", "tenant_id": "3d1f7595b468230304e0b73cecbcb081"}
        let read_only = "eyJhbGciOiJFZERTQSJ9.eyJzY29wZSI6InRlbmFudF9yZWFkX29ubHkiLCJ0ZW5hbnRfaWQiOiIzZDFmNzU5NWI0NjgyMzAzMDRlMGI3M2NlY2JjYjA4MSJ9.c2ln";
        assert_eq!(token_scope(read_only), Some(Scope::TenantReadOnly));

        // {"scope": "tenant", "tenant_id": "3d1f7595b468230304e0b73cecbcb081"}
        let read_write = "eyJhbGciOiJFZERTQSJ9.eyJzY29wZSI6InRlbmFudCIsInRlbmFudF9pZCI6IjNkMWY3NTk1YjQ2ODIzMDMwNGUwYjczY2VjYmNiMDgxIn0.c2ln";
        assert_eq!(token_scope(read_write), Some(Scope::Tenant));

        assert_eq!(token_scope("not a token"), None);
    }
}
