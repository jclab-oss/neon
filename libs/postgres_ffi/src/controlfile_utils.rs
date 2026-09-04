//!
//! Utilities for reading and writing the PostgreSQL control file.
//!
//! The PostgreSQL control file is one the first things that the PostgreSQL
//! server reads when it starts up. It indicates whether the server was shut
//! down cleanly, or if it crashed or was restored from online backup so that
//! WAL recovery needs to be performed. It also contains a copy of the latest
//! checkpoint record and its location in the WAL.
//!
//! The control file also contains fields for detecting whether the
//! data directory is compatible with a postgres binary. That includes
//! a version number, configuration options that can be set at
//! compilation time like the block size, and the platform's alignment
//! and endianness information. (The PostgreSQL on-disk file format is
//! not portable across platforms.)
//!
//! The control file is stored in the PostgreSQL data directory, as
//! `global/pg_control`. The data stored in it is designed to be smaller than
//! 512 bytes, on the assumption that it can be updated atomically. The actual
//! file is larger, 8192 bytes, but the rest of it is just filled with zeros.
//!
//! See src/include/catalog/pg_control.h in the PostgreSQL sources for more
//! information. You can use PostgreSQL's pg_controldata utility to view its
//! contents.
//!
use super::bindings::{ControlFileData, PG_CONTROL_FILE_SIZE};

use anyhow::{bail, Result};
use bytes::{Bytes, BytesMut};

/// Equivalent to sizeof(ControlFileData) in C
const SIZEOF_CONTROLDATA: usize = size_of::<ControlFileData>();

/// Byte offset of `pg_control_version` in the control file.
///
/// It is the second field, right after the uint64 `system_identifier`, and has never
/// moved in any version we support - which is what makes it safe to read before we
/// know which layout the rest of the file uses.
const PG_CONTROL_VERSION_OFFSET: usize = 8;

/// First `PG_CONTROL_VERSION` that uses the PG18 layout.
const PG_CONTROL_VERSION_PG18: u32 = 1800;

/// Offset of `crc` in each control file layout we know about.
///
/// PG18 inserted `bool default_char_signedness` ahead of `mock_authentication_nonce`
/// (catalog/pg_control.h), which pushed `crc` from 288 to 292. Everything before
/// offset 256 is identical across v14..v18, so the interesting fields are readable
/// either way - but the CRC check is not: it hashes `buf[0..crc_offset]` and then
/// compares against the value at that offset, so using the wrong layout both hashes
/// the wrong range and reads the wrong bytes.
const CRC_OFFSET_PRE_PG18: usize = 288;
const CRC_OFFSET_PG18: usize = 292;

/// Keep the two constants above honest: whichever layout *this* module was compiled
/// for, the struct bindgen produced must agree with it. If a future Postgres moves
/// `crc` again this fails to build here rather than silently mis-validating files.
const _: () = {
    let ours = std::mem::offset_of!(ControlFileData, crc);
    assert!(
        ours == CRC_OFFSET_PRE_PG18 || ours == CRC_OFFSET_PG18,
        "ControlFileData.crc moved; update CRC_OFFSET_* in controlfile_utils.rs"
    );
};

impl ControlFileData {
    /// Compute the offset of the `crc` field within the `ControlFileData` struct.
    /// Equivalent to offsetof(ControlFileData, crc) in C.
    const fn pg_control_crc_offset() -> usize {
        std::mem::offset_of!(ControlFileData, crc)
    }

    /// Offset of `crc` for the layout *this file* uses, which is not necessarily the
    /// one this module was compiled against.
    ///
    /// `postgres_ffi::ControlFileData` is re-exported from v14 and is used by callers
    /// that have no Postgres version in hand - `import_datadir`, and
    /// `import_pgdata::ControlFile`, which decodes precisely in order to *learn* the
    /// version. Before PG18 that was harmless because every layout agreed. It is not
    /// any more, so trust the file over the struct.
    fn crc_offset_of(buf: &[u8]) -> usize {
        let v = &buf[PG_CONTROL_VERSION_OFFSET..PG_CONTROL_VERSION_OFFSET + 4];
        let pg_control_version = u32::from_le_bytes(v.try_into().unwrap());

        if pg_control_version >= PG_CONTROL_VERSION_PG18 {
            CRC_OFFSET_PG18
        } else {
            CRC_OFFSET_PRE_PG18
        }
    }

    ///
    /// Interpret a slice of bytes as a Postgres control file.
    ///
    pub fn decode(buf: &[u8]) -> Result<ControlFileData> {
        use utils::bin_ser::LeSer;

        // Check that the slice has the expected size. The control file is
        // padded with zeros up to a 512 byte sector size, so accept a
        // larger size too, so that the caller can just the whole file
        // contents without knowing the exact size of the struct.
        if buf.len() < SIZEOF_CONTROLDATA {
            bail!("control file is too short");
        }

        // Compute the expected CRC of the content, using the layout the file itself
        // declares rather than the one we were compiled against.
        let OFFSETOF_CRC = Self::crc_offset_of(buf);
        let expectedcrc = crc32c::crc32c(&buf[0..OFFSETOF_CRC]);
        let filecrc = u32::from_le_bytes(buf[OFFSETOF_CRC..OFFSETOF_CRC + 4].try_into()?);

        // Use serde to deserialize the input as a ControlFileData struct.
        let controlfile = ControlFileData::des_prefix(buf)?;

        // Check the CRC
        if expectedcrc != filecrc {
            bail!(
                "invalid CRC in control file: expected {:08X}, was {:08X}",
                expectedcrc,
                filecrc
            );
        }

        Ok(controlfile)
    }

    ///
    /// Convert a struct representing a Postgres control file into raw bytes.
    ///
    /// The CRC is recomputed to match the contents of the fields.
    pub fn encode(&self) -> Bytes {
        use utils::bin_ser::LeSer;

        // Serialize into a new buffer.
        let b = self.ser().unwrap();

        // Recompute the CRC
        let OFFSETOF_CRC = Self::pg_control_crc_offset();
        let newcrc = crc32c::crc32c(&b[0..OFFSETOF_CRC]);

        let mut buf = BytesMut::with_capacity(PG_CONTROL_FILE_SIZE as usize);
        buf.extend_from_slice(&b[0..OFFSETOF_CRC]);
        buf.extend_from_slice(&newcrc.to_ne_bytes());
        // Fill the rest of the control file with zeros.
        buf.resize(PG_CONTROL_FILE_SIZE as usize, 0);

        buf.into()
    }
}
