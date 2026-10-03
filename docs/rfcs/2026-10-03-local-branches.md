# Local branches

Created on 2026-10-03
Implemented on 2026-10-03

## Summary

A local branch is a compute that starts from a timeline at a fixed LSN, and keeps
all its changes on its own disk instead of writing them to the timeline. Pages that
it hasn't modified are read from the pageserver at that LSN; everything else lives
in the compute's data directory. The pageserver and the safekeepers never see the
branch's changes, so the compute only needs read access to them.

## Motivation

Some branches are throwaway: a developer's scratch copy of production, a CI run, a
migration rehearsal. Their changes don't need to be durable in the storage layer,
replicated, or visible to anyone else, but today every write still goes through
the safekeepers and the pageserver, and costs storage there. Some users also want
to be sure that such a branch can't modify data in the storage layer, not even by
accident.

## Non Goals

- Reading a local branch's data from anywhere but its compute. If the compute's
  disk is lost, the branch is lost.
- Turning a local branch back into a regular branch.
- Read replicas of a local branch.

## Impacted components

- Storage auth (pageserver, safekeepers): a read-only token scope.
- compute: the `neon` extension stores modified pages locally.
- compute_ctl: a new compute mode, which keeps the data directory across restarts.
- neon_local: support for creating local branch endpoints.
- control plane (not in this repository): creates the timeline and the spec, see
  [Control plane contract](#control-plane-contract).

## Proposed implementation

### Read-only storage tokens

A new JWT scope, `tenant_read_only`, grants read access to a single tenant's data.
The pageserver accepts it for GetPage requests and basebackups (`pagestream_v2`,
`pagestream_v3`, `basebackup`, `fullbackup`, and the gRPC page service except
`LeaseLsn`). Everything else rejects it: LSN leases, because they hold back GC; the
management API; and the safekeepers.

It's a new scope rather than an extra claim on `tenant` tokens: pageservers and
safekeepers that predate it fail to parse it and reject the token, instead of
ignoring an unknown claim and granting read-write access. Existing `tenant` tokens
keep their meaning.

### The branch point

A local branch reads the timeline at a fixed LSN, its "local branch LSN". That LSN
must be the end of the timeline:

- Postgres can only start in read-write mode from a basebackup taken at the end of
  a timeline, where the pageserver knows the LSN of the previous WAL record
  (`PREV LSN` in `neon.signal`).
- The pageserver's GC keeps the data needed to read the end of a timeline, and the
  data at the branch point of a child timeline, but not much more than that. A
  local branch holds no LSN lease, it only reads with a read-only token.

So a local branch starts from a timeline that nothing writes to: typically a new
branch created for it, an "anchor" branch. The anchor holds no data of its own,
and as long as it exists, its ancestors' GC keeps the data at its branch point,
whether or not the local branch's compute is running. Deleting the anchor releases
that data.

Several local branches can share one anchor; they don't affect each other.

### Compute mode

`ComputeMode::Local(lsn)` in the compute spec makes compute_ctl start a local
branch at `lsn` on the spec's timeline. Compared to a primary:

- The spec must not have safekeepers, and the storage token, if any, must have
  the `tenant_read_only` scope. compute_ctl refuses the spec otherwise.
- compute_ctl takes the basebackup at `lsn`, doesn't sync safekeepers, and
  configures Postgres with `neon.compute_mode=local`, `neon.local_branch_lsn`,
  `fsync=on`, `full_page_writes=on`, `wal_log_hints=on`, and no `neon.safekeepers` or
  `synchronous_standby_names`, overriding the spec's settings.
- Once Postgres runs for the first time, compute_ctl runs a checkpoint and removes
  `neon.signal`, so that from then on, Postgres starts from its own data directory
  and replays its local WAL after a crash, like vanilla Postgres. The marker file
  `neon_local_branch.json` records which branch the data directory belongs to and
  whether that has happened. On restart, compute_ctl reuses the data directory if
  the marker matches the spec, starts from a new basebackup if the first start
  didn't complete, and refuses to start if the data directory belongs to a
  different local branch.

### Local storage in the `neon` extension

With `neon.compute_mode=local`, the `neon` smgr stores permanent relations locally
too (see `pgxn/neon/local_branch.c` for the details):

- For each relation fork that the branch modified, a data file at the path where
  md.c would store the fork's first segment holds the locally written blocks, all
  in one file, and a `.localmap` file next to it records whether the fork exists,
  how many of its blocks come from the pageserver, and a bitmap of which of those
  have been written locally.
- Reads of blocks that haven't been written locally go to the pageserver (and the
  LFC and the prefetch machinery), always at the local branch LSN. Other reads,
  writes, extensions and truncations use the local files.
- Forks without a map file haven't been modified locally and exist if they exist
  on the pageserver. Relations created locally, and the forks of relations dropped
  locally, have map files that say so, so that the pageserver's copy isn't used.
- WAL is only written to the local `pg_wal`. walproposer doesn't run, and WAL
  replay applies all records (Neon normally skips records for pages that aren't in
  shared buffers, because the pageserver has them).
- `CREATE DATABASE ... STRATEGY FILE_COPY` and `ALTER DATABASE ... SET TABLESPACE`
  copy database directories at the file level, which would miss the pages on the
  pageserver, so they're rejected. On PostgreSQL 14, `CREATE DATABASE` only has
  that strategy, so it's not supported there.
- `pg_database_size()` only counts what's stored locally.
- The pageserver only has relations in the default and global tablespaces (Neon
  doesn't support tablespaces outside regression tests), so relations in other
  tablespaces are always local.

### Reliability, failure modes and corner cases

Data files are fsync'd at checkpoints, via sync requests to the md.c sync handler,
which finds them by their md.c path. WAL replay restores pages written after the
last checkpoint from full-page images. The first local write of a block that came
from the pageserver is special: its bit in the map file must not become durable
before the data, or a crash could leave a hole where the pageserver's version of
the page used to be, which nothing in the WAL would restore if only hint bits had
changed. So that write fsyncs the data file before setting the bit. All map file
changes are durable immediately.

Protection against torn pages relies on every change to a page after a
checkpoint's redo pointer being preceded by a full-page image of it. Neon's
Postgres advances the page LSN when VACUUM sets the all-visible flag, without a
full-page image unless hint bits are WAL-logged, so a local branch runs with
`wal_log_hints=on`.

`smgrtruncate()` is called in a critical section, so the truncate path doesn't do
network I/O, and allocates memory in a context that allows it.

If something writes to the anchor branch, the local branch keeps working, but a
compute that hasn't completed its first start can't take a basebackup at the local
branch LSN in read-write mode anymore, and GC may eventually remove the data that
the local branch reads. neon_local refuses to run a primary on a timeline with a
running local branch.

### Security implications

A local branch's compute can't modify anything in the storage layer: its token
only allows reading the tenant's data, and the pageserver doesn't let it hold back
GC. It can still read all of the tenant's timelines, like any compute of the
tenant.

### Performance

Pages that the branch hasn't modified are read like in any other compute. The
first local write of a page that came from the pageserver costs two fsyncs (the
data, then the map file), which makes write-heavy workloads that touch many such
pages slower; later writes of the same page don't. Commits wait for a local WAL
fsync instead of the safekeepers.

### Future work

- Batch the fsyncs of first writes, e.g. by WAL-logging a full-page image of the
  page before its first local write, and syncing the map files at checkpoints.
- Cache the map files' state, instead of reading it on each I/O.

## Control plane contract

To create a local branch, the control plane:

1. Creates an anchor branch at the desired point, e.g. the end of the parent branch.
2. Creates a compute spec with `mode: {"Local": "<end LSN of the anchor>"}`, the
   anchor's timeline, no safekeepers, and a storage token with the
   `tenant_read_only` scope for the tenant.
3. Gives the compute a persistent volume for its data directory, and keeps using it
   when the compute restarts or moves.
4. Doesn't start computes that write to the anchor.
5. Deletes the anchor when the local branch is deleted.

With neon_local:

```console
$ neon_local timeline branch --branch-name scratch --ancestor-branch-name main
$ neon_local endpoint create ep-scratch --branch-name scratch --local-branch
$ neon_local endpoint start ep-scratch
```
