use crate::PgMajorVersion;

pub const MY_PGVERSION: PgMajorVersion = PgMajorVersion::PG18;

// Every constant below was checked against the PG18 headers and is unchanged from
// PG17, so they are re-exported rather than duplicated - the same thing v16 does for
// PGDATA_SUBDIRS. Verified against vendor/postgres-v18:
//
//   XACT_XINFO_HAS_DROPPED_STATS   src/include/access/xact.h:196            (1U << 8)
//   XLOG_DBASE_*                   src/include/commands/dbcommands_xlog.h:21-23
//   BKPIMAGE_*                     src/include/access/xlogrecord.h:157-163
//   XLOG_HEAP2_PRUNE_*             src/include/access/heapam_xlog.h:60-62
//   XLOG_OVERWRITE_CONTRECORD      src/include/catalog/pg_control.h:81       (0xD0)
//   XLOG_CHECKPOINT_REDO           src/include/catalog/pg_control.h:82       (0xE0)
//   PGDATA_SUBDIRS                 src/bin/initdb/initdb.c - same 23 entries
//   SIZEOF_RELMAPFILE              relmapper.c: RelMapFile is still
//                                  int32 + int32 + RelMapping[64] + pg_crc32c,
//                                  with RelMapping = Oid + RelFileNumber and
//                                  MAX_MAPPINGS = 64, i.e. 4+4+64*8+4 = 524.
pub use super::super::v17::bindings::{
    BKPIMAGE_APPLY, BKPIMAGE_COMPRESS_LZ4, BKPIMAGE_COMPRESS_PGLZ, BKPIMAGE_COMPRESS_ZSTD,
    PGDATA_SUBDIRS, SIZEOF_RELMAPFILE, XACT_XINFO_HAS_DROPPED_STATS, XLOG_CHECKPOINT_REDO,
    XLOG_DBASE_CREATE_FILE_COPY, XLOG_DBASE_CREATE_WAL_LOG, XLOG_DBASE_DROP,
    XLOG_HEAP2_PRUNE_ON_ACCESS, XLOG_HEAP2_PRUNE_VACUUM_CLEANUP, XLOG_HEAP2_PRUNE_VACUUM_SCAN,
    XLOG_OVERWRITE_CONTRECORD, bkpimg_is_compressed,
};
