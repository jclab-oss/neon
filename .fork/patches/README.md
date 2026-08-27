# Local patches to vendor/postgres-v18

These cannot live in the submodule gitlink: they are changes to
`neondatabase/postgres`, which we cannot push to, so the v18 work carries them
here and applies them on top of the pinned commit.

Apply with:

```sh
git -C vendor/postgres-v18 apply ../../.fork/patches/v18-lwlsn-block-hooks.patch
```

Drop a patch once an equivalent lands on `REL_18_STABLE_neon`.

## v18-lwlsn-block-hooks.patch

Against `a616eefea763784218a67f81395ba95bc2cf7c4d` (PostgreSQL 18.2).

`REL_18_STABLE_neon` carries three of the six last-written-LSN hooks
(`set_lwlsn_db_hook`, `set_lwlsn_relation_hook`, `set_max_lwlsn_hook`) but not
the three block-level ones, which is what `pgxn/neon/neon_lwlsncache.c` needs.
PG18 reworked the buffer read path for AIO, which is presumably why they were
left behind. Ported from v17:

| file | change |
| --- | --- |
| `src/include/access/xlog.h` | `set_lwlsn_block_hook_type`, `set_lwlsn_block_range_hook_type`, `set_lwlsn_block_v_hook_type` typedefs + externs |
| `src/backend/access/transam/xlog.c` | the three hook variable definitions |
| `src/include/storage/lwlocklist.h` | `LastWrittenLsn` lock. **Slot 54, not 53** - PG18 took 53 for `AioWorkerSubmissionQueue` |
| `src/backend/utils/activity/wait_event_names.txt` | matching wait event; the file must list predefined LWLocks in the same order as `lwlocklist.h` |
| `src/backend/access/gist/gistbuild.c` | the `set_lwlsn_block_hook` / `set_lwlsn_relation_hook` call site after the GiST root page is written |

Verified: `make postgres-install-v18` exits 0, `build/v18/src/include/storage/lwlocknames.h`
gains `LastWrittenLsn`, and `postgres --version` reports 18.2. Applying it took
the `neon-pg-ext-v18` error count from 18 distinct errors to 9, removing every
LwLSN one.
