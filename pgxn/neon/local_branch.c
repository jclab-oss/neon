/*-------------------------------------------------------------------------
 *
 * local_branch.c
 *	  Local branches: computes that keep their writes on local disk.
 *
 * A local branch starts from a pageserver timeline at a fixed LSN, the
 * "local branch LSN". Instead of streaming WAL to the safekeepers, the compute
 * writes WAL only to its local pg_wal, and keeps the pages that it modifies in
 * local files. Pages that it hasn't modified are read from the pageserver,
 * always at the local branch LSN. The pageserver never sees any of the
 * branch's changes, so the compute only needs read access to it.
 *
 * Local storage
 * -------------
 *
 * Temporary and unlogged relations are stored by md.c, as usual. For each
 * fork of a permanent relation that the branch has modified, there are two
 * local files:
 *
 * - The data file, at the path where md.c would store the first segment of
 *   the fork (e.g. "base/5/16384"). Unlike md.c, all blocks are stored in this
 *   one file, block N at offset N * BLCKSZ. Blocks that haven't been written
 *   locally are holes.
 *
 * - The map file, at the same path with a ".localmap" suffix. It records
 *   whether the fork exists, how many blocks of it come from the pageserver
 *   ('remote_nblocks'), and a bitmap of which of those blocks have been
 *   written locally. A block below 'remote_nblocks' is read from the data file
 *   if its bit is set, and from the pageserver otherwise. Blocks at or above
 *   'remote_nblocks' are always read from the data file. The size of the fork
 *   is the larger of 'remote_nblocks' and the size of the data file.
 *
 * A fork without a map file hasn't been modified by the branch, and exists if
 * it exists on the pageserver. There are two exceptions: if the map file of
 * the relation's main fork says that the relation was created or dropped
 * locally, its other forks don't exist on the pageserver either; and the
 * pageserver only has relations in the default and global tablespaces.
 *
 * Map files are created when a relation fork is created, or when a fork that
 * comes from the pageserver is modified for the first time. When a relation
 * that might exist on the pageserver is dropped, the map file of its main
 * fork is kept as a tombstone, so that the pageserver's copy of the relation
 * doesn't come back.
 *
 * Durability
 * ----------
 *
 * The data files are fsync'd at checkpoints, like md.c files: we register
 * sync requests with the md.c sync handler, which finds the files by their
 * md.c path. That is enough for blocks that have been written locally before.
 * If we crash before the next checkpoint, WAL replay restores them from
 * full-page images, so full_page_writes must be on. And so must wal_log_hints:
 * Neon's Postgres advances the page LSN when it sets the all-visible flag, and
 * without wal_log_hints, it doesn't WAL-log a full-page image for that, so the
 * page's later changes until the next checkpoint wouldn't have one either.
 *
 * The first local write of a block that comes from the pageserver needs more
 * care. If its bit in the map file became durable before the data, a crash
 * could leave a hole in the data file where the pageserver's version of the
 * page used to be, and if only hint bits had been changed, nothing in the WAL
 * would restore the page. So on the first write of such a block, we fsync the
 * data file before setting the bit. Changes to the map files are always made
 * durable immediately.
 *
 * Map file updates are serialized by a small array of LWLocks, partitioned
 * by relation fork. Reads don't need locking: the buffer manager guarantees
 * that a block isn't read while it's being written.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog.h"
#include "catalog/pg_tablespace_d.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "port/pg_iovec.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/md.h"
#include "storage/sync.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "bitmap.h"
#include "communicator.h"
#include "local_branch.h"
#include "neon.h"
#include "pagestore_client.h"

bool		neon_local_branch = false;
XLogRecPtr	local_branch_lsn = InvalidXLogRecPtr;

static char *local_branch_lsn_str = "";

#define LOCAL_MAP_SUFFIX ".localmap"
#define LOCAL_MAP_MAGIC 0x4e4c4d31	/* "NLM1" */
#define LOCAL_MAP_VERSION 1
/* The bitmap starts at this offset, so that the header fits in one sector */
#define LOCAL_MAP_BITMAP_OFFSET 512

/* The fork exists. If not set, the map file is a tombstone. */
#define LOCAL_MAP_EXISTS			0x01
/* Blocks below remote_nblocks that aren't marked come from the pageserver */
#define LOCAL_MAP_REMOTE_BACKED		0x02

typedef struct
{
	uint32		magic;
	uint32		version;
	uint32		flags;
	BlockNumber remote_nblocks;
} LocalMapHeader;

typedef enum
{
	/* Not modified by the branch. The pageserver has it, if it exists. */
	LB_REMOTE,
	/* Has a map file, see LocalForkMeta */
	LB_LOCAL,
	/* Doesn't exist: dropped, or a missing fork of a locally created relation */
	LB_NONEXISTENT,
} LocalForkState;

typedef struct
{
	bool		remote_backed;
	BlockNumber remote_nblocks;
} LocalForkMeta;

#define LOCAL_BRANCH_NUM_LOCKS 64

static LWLockPadded *local_branch_locks;

/* Memory context for smgrtruncate(), which is called in a critical section */
static MemoryContext local_branch_truncate_cxt = NULL;

static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

static void local_branch_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
										bool readOnlyTree, ProcessUtilityContext context,
										ParamListInfo params, QueryEnvironment *queryEnv,
										DestReceiver *dest, QueryCompletion *qc);

/*
 * Initialization at postmaster startup, after the libpagestore GUCs have been
 * defined.
 */
void
pg_init_local_branch(void)
{
	uint32		hi,
				lo;

	DefineCustomStringVariable("neon.local_branch_lsn",
							   "LSN of the pageserver timeline that this local branch starts from",
							   "Only used with neon.compute_mode=local.",
							   &local_branch_lsn_str,
							   "",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);

	if (neon_compute_mode != CP_MODE_LOCAL)
	{
		if (local_branch_lsn_str[0] != '\0')
			ereport(ERROR,
					(errmsg(NEON_TAG "neon.local_branch_lsn can only be set with neon.compute_mode=local")));
		return;
	}

	if (sscanf(local_branch_lsn_str, "%X/%X", &hi, &lo) != 2)
		ereport(ERROR,
				(errmsg(NEON_TAG "invalid neon.local_branch_lsn \"%s\"", local_branch_lsn_str),
				 errhint("neon.compute_mode=local requires neon.local_branch_lsn to be set.")));
	local_branch_lsn = ((uint64) hi) << 32 | lo;
	if (local_branch_lsn == InvalidXLogRecPtr)
		ereport(ERROR,
				(errmsg(NEON_TAG "invalid neon.local_branch_lsn \"%s\"", local_branch_lsn_str)));

	if (pageserver_connstring[0] == '\0')
		ereport(ERROR,
				(errmsg(NEON_TAG "neon.compute_mode=local requires neon.pageserver_connstring to be set")));
	if (debug_compare_local)
		ereport(ERROR,
				(errmsg(NEON_TAG "neon.debug_compare_local can't be used with neon.compute_mode=local")));

	/*
	 * Losing a write after a crash would lose data that exists nowhere else.
	 * wal_log_hints is needed because Neon's Postgres advances the page LSN
	 * when it sets the all-visible flag, without a full-page image unless
	 * hint bits are WAL-logged, which would leave the page unprotected from
	 * torn writes until the next checkpoint.
	 */
	if (!enableFsync || !fullPageWrites || !wal_log_hints)
		ereport(WARNING,
				(errmsg(NEON_TAG "fsync, full_page_writes and wal_log_hints should be on in a local branch"),
				 errdetail("A crash could corrupt the branch.")));

	neon_local_branch = true;

	/* The pageserver doesn't know the size of local databases */
	dbsize_hook = NULL;

	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = local_branch_ProcessUtility;
}

void
LocalBranchShmemRequest(void)
{
	if (neon_local_branch)
		RequestNamedLWLockTranche("neon_local_branch", LOCAL_BRANCH_NUM_LOCKS);
}

void
LocalBranchShmemInit(void)
{
	if (neon_local_branch)
		local_branch_locks = GetNamedLWLockTranche("neon_local_branch");
}

/*
 * Initialization in each process that uses the smgr.
 */
void
local_branch_init_backend(void)
{
	local_branch_truncate_cxt = AllocSetContextCreate(TopMemoryContext,
													  "local branch truncate",
													  ALLOCSET_SMALL_SIZES);
	MemoryContextAllowInCriticalSection(local_branch_truncate_cxt, true);
}

/*
 * Commands that copy database directories at the file level would only copy
 * what's stored locally, so they're not supported in a local branch.
 */
static void
local_branch_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
							bool readOnlyTree, ProcessUtilityContext context,
							ParamListInfo params, QueryEnvironment *queryEnv,
							DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parseTree = pstmt->utilityStmt;
	ListCell   *option;

	if (IsA(parseTree, CreatedbStmt))
	{
#if PG_MAJORVERSION_NUM < 15
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CREATE DATABASE is not supported in a local branch on PostgreSQL 14")));
#else
		foreach(option, ((CreatedbStmt *) parseTree)->options)
		{
			DefElem    *defel = lfirst_node(DefElem, option);

			if (strcmp(defel->defname, "strategy") == 0 &&
				pg_strcasecmp(defGetString(defel), "file_copy") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("the FILE_COPY strategy of CREATE DATABASE is not supported in a local branch")));
		}
#endif
	}
	else if (IsA(parseTree, AlterDatabaseStmt))
	{
		foreach(option, ((AlterDatabaseStmt *) parseTree)->options)
		{
			DefElem    *defel = lfirst_node(DefElem, option);

			if (strcmp(defel->defname, "tablespace") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("ALTER DATABASE SET TABLESPACE is not supported in a local branch")));
		}
	}

	if (prev_ProcessUtility_hook)
		prev_ProcessUtility_hook(pstmt, queryString, readOnlyTree, context,
								 params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* ----------------------------------------------------------------
 * Map files
 * ----------------------------------------------------------------
 */

static char *
local_map_path(NRelFileInfo rinfo, ForkNumber forknum)
{
	char	   *datapath = relpathperm(rinfo, forknum);
	char	   *path = psprintf("%s" LOCAL_MAP_SUFFIX, datapath);

	pfree(datapath);
	return path;
}

static LWLock *
local_map_lock(NRelFileInfo rinfo, ForkNumber forknum)
{
	uint32		hash;

	hash = NInfoGetRelNumber(rinfo) ^ (NInfoGetDbOid(rinfo) * 31) ^ (forknum * 7919);
	return &local_branch_locks[hash % LOCAL_BRANCH_NUM_LOCKS].lock;
}

typedef enum
{
	LOCAL_MAP_FOUND,
	LOCAL_MAP_NOT_FOUND,
	LOCAL_MAP_FAILED,			/* only if elevel < ERROR */
} LocalMapReadResult;

/*
 * Read the header of a map file. Failures are reported at 'elevel'.
 */
static LocalMapReadResult
local_map_read_header_ext(NRelFileInfo rinfo, ForkNumber forknum, LocalMapHeader *hdr,
						  int elevel)
{
	char	   *path = local_map_path(rinfo, forknum);
	LocalMapReadResult result = LOCAL_MAP_FAILED;
	int			fd;
	int			nread;

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT)
			result = LOCAL_MAP_NOT_FOUND;
		else
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		pfree(path);
		return result;
	}

	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
	nread = pg_pread(fd, hdr, sizeof(*hdr), 0);
	pgstat_report_wait_end();
	if (nread < 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	else if (nread != sizeof(*hdr) || hdr->magic != LOCAL_MAP_MAGIC ||
			 hdr->version != LOCAL_MAP_VERSION)
		ereport(elevel,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid local branch map file \"%s\"", path)));
	else
		result = LOCAL_MAP_FOUND;

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
		result = LOCAL_MAP_FAILED;
	}
	pfree(path);
	return result;
}

/*
 * Read the header of a map file. Returns false if there is no map file.
 */
static bool
local_map_read_header(NRelFileInfo rinfo, ForkNumber forknum, LocalMapHeader *hdr)
{
	return local_map_read_header_ext(rinfo, forknum, hdr, ERROR) == LOCAL_MAP_FOUND;
}

/*
 * Atomically create or replace a map file, with an empty bitmap. The caller
 * must hold the lock for the fork. Failures are reported at 'elevel'.
 */
static bool
local_map_write_ext(NRelFileInfo rinfo, ForkNumber forknum, uint32 flags,
					BlockNumber remote_nblocks, int elevel)
{
	char	   *path = local_map_path(rinfo, forknum);
	char	   *tmppath = psprintf("%s.tmp", path);
	LocalMapHeader hdr;
	bool		ok = false;
	int			fd;

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = LOCAL_MAP_MAGIC;
	hdr.version = LOCAL_MAP_VERSION;
	hdr.flags = flags;
	hdr.remote_nblocks = remote_nblocks;

	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));
		goto out;
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (pg_pwrite(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr))
	{
		pgstat_report_wait_end();
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", tmppath)));
		CloseTransientFile(fd);
		goto out;
	}
	pgstat_report_wait_end();

	/* The bitmap is all zeros, so leave it as a hole */
	if (remote_nblocks > 0 &&
		ftruncate(fd, LOCAL_MAP_BITMAP_OFFSET + (remote_nblocks + 7) / 8) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", tmppath)));
		CloseTransientFile(fd);
		goto out;
	}

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));
		goto out;
	}

	/* fsyncs the file and its directory */
	ok = durable_rename(tmppath, path, elevel) == 0;

out:
	pfree(tmppath);
	pfree(path);
	return ok;
}

static void
local_map_write(NRelFileInfo rinfo, ForkNumber forknum, uint32 flags,
				BlockNumber remote_nblocks)
{
	(void) local_map_write_ext(rinfo, forknum, flags, remote_nblocks, ERROR);
}

/*
 * Durably remove a map file, if it exists. The caller must hold the lock for
 * the fork.
 */
static void
local_map_remove(NRelFileInfo rinfo, ForkNumber forknum, int elevel)
{
	char	   *path = local_map_path(rinfo, forknum);

	if (unlink(path) == 0)
	{
		char	   *dir = pstrdup(path);

		get_parent_directory(dir);
		fsync_fname_ext(dir, true, false, elevel);
		pfree(dir);
	}
	else if (errno != ENOENT)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));
	pfree(path);
}

/*
 * Could the pageserver have the relation? Neon doesn't support tablespaces
 * (CREATE TABLESPACE is only allowed in regression tests), so it only has
 * relations in the default and global tablespaces.
 */
static bool
could_be_remote(NRelFileInfo rinfo)
{
	return NInfoGetSpcOid(rinfo) == DEFAULTTABLESPACE_OID ||
		NInfoGetSpcOid(rinfo) == GLOBALTABLESPACE_OID;
}

/*
 * Look up the local state of a relation fork. Returns false if that failed,
 * which is only possible if elevel < ERROR.
 */
static bool
local_fork_lookup_ext(NRelFileInfo rinfo, ForkNumber forknum, LocalForkMeta *meta,
					  LocalForkState *state, int elevel)
{
	LocalMapHeader hdr;

	switch (local_map_read_header_ext(rinfo, forknum, &hdr, elevel))
	{
		case LOCAL_MAP_FOUND:
			if (!(hdr.flags & LOCAL_MAP_EXISTS))
				*state = LB_NONEXISTENT;
			else
			{
				meta->remote_backed = (hdr.flags & LOCAL_MAP_REMOTE_BACKED) != 0;
				meta->remote_nblocks = hdr.remote_nblocks;
				*state = LB_LOCAL;
			}
			return true;
		case LOCAL_MAP_NOT_FOUND:
			break;
		case LOCAL_MAP_FAILED:
			return false;
	}

	if (!could_be_remote(rinfo))
	{
		*state = LB_NONEXISTENT;
		return true;
	}

	/*
	 * If the relation was created or dropped locally, the pageserver doesn't
	 * have any of its forks.
	 */
	*state = LB_REMOTE;
	if (forknum != MAIN_FORKNUM)
	{
		switch (local_map_read_header_ext(rinfo, MAIN_FORKNUM, &hdr, elevel))
		{
			case LOCAL_MAP_FOUND:
				if ((hdr.flags & (LOCAL_MAP_EXISTS | LOCAL_MAP_REMOTE_BACKED)) !=
					(LOCAL_MAP_EXISTS | LOCAL_MAP_REMOTE_BACKED))
					*state = LB_NONEXISTENT;
				break;
			case LOCAL_MAP_NOT_FOUND:
				break;
			case LOCAL_MAP_FAILED:
				return false;
		}
	}
	return true;
}

/*
 * Look up the local state of a relation fork.
 */
static LocalForkState
local_fork_lookup(NRelFileInfo rinfo, ForkNumber forknum, LocalForkMeta *meta)
{
	LocalForkState state;

	(void) local_fork_lookup_ext(rinfo, forknum, meta, &state, ERROR);
	return state;
}

/*
 * Mark which of the blocks [blkno, blkno + nblocks) are stored locally.
 */
static void
local_map_get_local_blocks(NRelFileInfo rinfo, ForkNumber forknum,
						   const LocalForkMeta *meta, BlockNumber blkno,
						   BlockNumber nblocks, bits8 *local)
{
	BlockNumber nremote;
	BlockNumber first_byte;
	BlockNumber last_byte;
	uint8		bitmap[PG_IOV_MAX / 8 + 2];
	char	   *path;
	int			fd;
	int			nread;

	Assert(nblocks <= PG_IOV_MAX);
	memset(local, 0, (nblocks + 7) / 8);

	/* Blocks at or above remote_nblocks are always local */
	nremote = 0;
	if (meta->remote_backed && blkno < meta->remote_nblocks)
		nremote = Min(nblocks, meta->remote_nblocks - blkno);
	for (BlockNumber i = nremote; i < nblocks; i++)
		BITMAP_SET(local, i);
	if (nremote == 0)
		return;

	first_byte = blkno / 8;
	last_byte = (blkno + nremote - 1) / 8;

	path = local_map_path(rinfo, forknum);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
	nread = pg_pread(fd, bitmap, last_byte - first_byte + 1,
					 LOCAL_MAP_BITMAP_OFFSET + first_byte);
	pgstat_report_wait_end();
	if (nread != last_byte - first_byte + 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	pfree(path);

	for (BlockNumber i = 0; i < nremote; i++)
	{
		BlockNumber b = blkno + i;

		if (bitmap[b / 8 - first_byte] & (1 << (b % 8)))
			BITMAP_SET(local, i);
	}
}

/*
 * Durably mark blocks of a remote-backed fork as stored locally. The data must
 * already be durable.
 */
static void
local_map_mark_local_blocks(NRelFileInfo rinfo, ForkNumber forknum,
							BlockNumber blkno, BlockNumber nblocks, const bits8 *mark)
{
	LWLock	   *lock = local_map_lock(rinfo, forknum);
	BlockNumber first = InvalidBlockNumber;
	BlockNumber last = InvalidBlockNumber;
	BlockNumber first_byte;
	BlockNumber last_byte;
	int			len;
	uint8		bitmap[PG_IOV_MAX / 8 + 2];
	char	   *path;
	int			fd;

	Assert(nblocks <= PG_IOV_MAX);

	/* Only touch the bytes of the marked blocks, which are all in the bitmap */
	for (BlockNumber i = 0; i < nblocks; i++)
	{
		if (BITMAP_ISSET(mark, i))
		{
			if (first == InvalidBlockNumber)
				first = blkno + i;
			last = blkno + i;
		}
	}
	if (first == InvalidBlockNumber)
		return;
	first_byte = first / 8;
	last_byte = last / 8;
	len = last_byte - first_byte + 1;

	path = local_map_path(rinfo, forknum);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
	if (pg_pread(fd, bitmap, len, LOCAL_MAP_BITMAP_OFFSET + first_byte) != len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", path)));
	pgstat_report_wait_end();

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		BlockNumber b = blkno + i;

		if (BITMAP_ISSET(mark, i))
			bitmap[b / 8 - first_byte] |= 1 << (b % 8);
	}

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
	if (pg_pwrite(fd, bitmap, len, LOCAL_MAP_BITMAP_OFFSET + first_byte) != len)
	{
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", path)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	LWLockRelease(lock);
	pfree(path);
}

/*
 * Durably lower the number of blocks of a fork that come from the pageserver.
 */
static void
local_map_shrink_remote(NRelFileInfo rinfo, ForkNumber forknum, BlockNumber nblocks)
{
	LWLock	   *lock = local_map_lock(rinfo, forknum);
	char	   *path = local_map_path(rinfo, forknum);
	LocalMapHeader hdr;
	int			fd;

	LWLockAcquire(lock, LW_EXCLUSIVE);

	if (!local_map_read_header(rinfo, forknum, &hdr))
		elog(ERROR, "local branch map file \"%s\" is missing", path);

	if (nblocks < hdr.remote_nblocks)
	{
		hdr.remote_nblocks = nblocks;

		/* The header is within one sector, so it's written atomically */
		fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
		if (fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
		if (pg_pwrite(fd, &hdr, sizeof(hdr), 0) != sizeof(hdr))
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m", path)));
		}
		pgstat_report_wait_end();
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_SYNC);
		if (pg_fsync(fd) != 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", path)));
		pgstat_report_wait_end();
		if (CloseTransientFile(fd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", path)));
	}

	LWLockRelease(lock);
	pfree(path);
}

/* ----------------------------------------------------------------
 * Data files
 * ----------------------------------------------------------------
 */

static int
local_data_open(NRelFileInfo rinfo, ForkNumber forknum, int flags, char **path)
{
	int			fd;

	*path = relpathperm(rinfo, forknum);
	fd = OpenTransientFile(*path, flags | PG_BINARY);
	if (fd < 0 && !(errno == ENOENT && !(flags & O_CREAT)))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", *path)));
	return fd;
}

static void
local_data_close(int fd, char *path)
{
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
	pfree(path);
}

static void
local_data_fsync(int fd, const char *path)
{
	pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path)));
	pgstat_report_wait_end();
}

static BlockNumber
local_data_nblocks(NRelFileInfo rinfo, ForkNumber forknum)
{
	char	   *path = relpathperm(rinfo, forknum);
	struct stat st;
	BlockNumber nblocks = 0;

	if (stat(path, &st) == 0)
		nblocks = st.st_size / BLCKSZ;
	else if (errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	pfree(path);
	return nblocks;
}

/*
 * Ask the checkpointer to fsync the data file at the next checkpoint. The md.c
 * sync handler finds it at the path of the first segment of the fork.
 */
static void
local_data_register_sync(NRelFileInfo rinfo, ForkNumber forknum)
{
	FileTag		tag;

	memset(&tag, 0, sizeof(tag));
	tag.handler = SYNC_HANDLER_MD;
	tag.forknum = forknum;
#if PG_MAJORVERSION_NUM >= 16
	tag.rlocator = rinfo;
#else
	tag.rnode = rinfo;
#endif
	tag.segno = 0;

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* retryOnError */ ))
	{
		char	   *path;
		int			fd;

		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		fd = local_data_open(rinfo, forknum, O_RDWR, &path);
		if (fd >= 0)
		{
			local_data_fsync(fd, path);
			local_data_close(fd, path);
		}
		else
			pfree(path);
	}
}

static void
local_data_read(NRelFileInfo rinfo, ForkNumber forknum, BlockNumber blkno,
				void **buffers, BlockNumber nblocks, const bits8 *mask)
{
	char	   *path;
	int			fd;
	bool		any = false;

	for (BlockNumber i = 0; i < nblocks; i++)
		any |= BITMAP_ISSET(mask, i) != 0;
	if (!any)
		return;

	fd = local_data_open(rinfo, forknum, O_RDONLY, &path);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		int			nread;

		if (!BITMAP_ISSET(mask, i))
			continue;

		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_READ);
		nread = pg_pread(fd, buffers[i], BLCKSZ, (off_t) (blkno + i) * BLCKSZ);
		pgstat_report_wait_end();
		if (nread < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read block %u in file \"%s\": %m",
							blkno + i, path)));
		if (nread != BLCKSZ)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read block %u in file \"%s\": read only %d of %d bytes",
							blkno + i, path, nread, BLCKSZ)));
	}

	local_data_close(fd, path);
}

/*
 * Write blocks to the data file. If 'sync' is set, they are durable on
 * return.
 */
static void
local_data_write(NRelFileInfo rinfo, ForkNumber forknum, BlockNumber blkno,
				 const void **buffers, BlockNumber nblocks, bool sync)
{
	char	   *path;
	int			fd;
	bool		created = false;

	fd = local_data_open(rinfo, forknum, O_RDWR, &path);
	if (fd < 0)
	{
		pfree(path);
		fd = local_data_open(rinfo, forknum, O_RDWR | O_CREAT, &path);
		created = true;
	}

	for (BlockNumber i = 0; i < nblocks; i++)
	{
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_WRITE);
		if (pg_pwrite(fd, buffers[i], BLCKSZ, (off_t) (blkno + i) * BLCKSZ) != BLCKSZ)
		{
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write block %u in file \"%s\": %m",
							blkno + i, path),
					 errhint("Check free disk space.")));
		}
		pgstat_report_wait_end();
	}

	if (sync)
	{
		local_data_fsync(fd, path);
		if (created)
		{
			char	   *dir = pstrdup(path);

			get_parent_directory(dir);
			fsync_fname(dir, true);
			pfree(dir);
		}
	}

	local_data_close(fd, path);
}

/* Set the size of the data file, if it's not that large already */
static void
local_data_extend(NRelFileInfo rinfo, ForkNumber forknum, BlockNumber nblocks)
{
	char	   *path;
	int			fd;
	struct stat st;

	fd = local_data_open(rinfo, forknum, O_RDWR | O_CREAT, &path);
	if (fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	if (st.st_size < (off_t) nblocks * BLCKSZ)
	{
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_EXTEND);
		if (ftruncate(fd, (off_t) nblocks * BLCKSZ) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m", path),
					 errhint("Check free disk space.")));
		pgstat_report_wait_end();
	}
	local_data_close(fd, path);
}

/*
 * Truncate the data file, if it's larger. Returns false if there is no data
 * file.
 */
static bool
local_data_truncate(NRelFileInfo rinfo, ForkNumber forknum, BlockNumber nblocks)
{
	char	   *path;
	int			fd;
	struct stat st;

	fd = local_data_open(rinfo, forknum, O_RDWR, &path);
	if (fd < 0)
	{
		pfree(path);
		return false;
	}
	if (fstat(fd, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	if (st.st_size > (off_t) nblocks * BLCKSZ)
	{
		pgstat_report_wait_start(WAIT_EVENT_DATA_FILE_TRUNCATE);
		if (ftruncate(fd, (off_t) nblocks * BLCKSZ) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not truncate file \"%s\" to %u blocks: %m",
							path, nblocks)));
		pgstat_report_wait_end();
	}
	local_data_close(fd, path);
	return true;
}

/* ----------------------------------------------------------------
 * smgr functions
 * ----------------------------------------------------------------
 */

static BlockNumber
remote_nblocks(NRelFileInfo rinfo, ForkNumber forknum)
{
	neon_request_lsns request_lsns;

	neon_get_request_lsns(rinfo, forknum, REL_METADATA_PSEUDO_BLOCKNO, &request_lsns, 1);
	return communicator_nblocks(rinfo, forknum, &request_lsns);
}

static bool
remote_exists(NRelFileInfo rinfo, ForkNumber forknum)
{
	neon_request_lsns request_lsns;
	BlockNumber nblocks;

	/* Only the size of forks that exist is cached */
	if (get_cached_relsize(rinfo, forknum, &nblocks))
		return true;

	neon_get_request_lsns(rinfo, forknum, REL_METADATA_PSEUDO_BLOCKNO, &request_lsns, 1);
	return communicator_exists(rinfo, forknum, &request_lsns);
}

/*
 * Get the local state of a fork that is about to be modified. Forks that come
 * from the pageserver get a map file now. If the caller knows the size of the
 * fork, it can pass it as 'known_nblocks', otherwise InvalidBlockNumber.
 */
static void
local_fork_for_write(SMgrRelation reln, ForkNumber forknum, LocalForkMeta *meta,
					 BlockNumber known_nblocks)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LWLock	   *lock;
	BlockNumber nblocks = known_nblocks;

	switch (local_fork_lookup(rinfo, forknum, meta))
	{
		case LB_LOCAL:
			return;
		case LB_NONEXISTENT:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg(NEON_TAG "relation %u/%u/%u fork %u does not exist",
							RelFileInfoFmt(rinfo), forknum)));
			break;
		case LB_REMOTE:
			break;
	}

	/*
	 * The fork hasn't been modified yet, so its size is the size on the
	 * pageserver. Get it before taking the lock, it may involve network I/O.
	 */
	if (nblocks == InvalidBlockNumber &&
		!get_cached_relsize(rinfo, forknum, &nblocks))
		nblocks = remote_nblocks(rinfo, forknum);

	/* Like mdcreate(), make sure that the database's directory exists */
	TablespaceCreateDbspace(NInfoGetSpcOid(rinfo), NInfoGetDbOid(rinfo), false);

	lock = local_map_lock(rinfo, forknum);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	if (local_fork_lookup(rinfo, forknum, meta) == LB_REMOTE)
	{
		local_map_write(rinfo, forknum, LOCAL_MAP_EXISTS | LOCAL_MAP_REMOTE_BACKED, nblocks);
		meta->remote_backed = true;
		meta->remote_nblocks = nblocks;
	}
	LWLockRelease(lock);
}

/*
 * Is a write to a relation of unknown persistence a write to a permanent
 * relation? Otherwise, it's a write to an unlogged relation, which md.c
 * stores.
 */
bool
local_branch_is_permanent(SMgrRelation reln, ForkNumber forknum)
{
	LocalForkMeta meta;

	if (local_fork_lookup(InfoFromSMgrRel(reln), forknum, &meta) == LB_LOCAL)
		return true;
	return !mdexists(reln, forknum);
}

bool
local_branch_exists(SMgrRelation reln, ForkNumber forknum)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;

	switch (local_fork_lookup(rinfo, forknum, &meta))
	{
		case LB_LOCAL:
			return true;
		case LB_NONEXISTENT:
			return false;
		case LB_REMOTE:
			break;
	}

	/*
	 * \d+ on a view calls smgrexists with 0/0/0 relfilenode, see
	 * neon_exists().
	 */
	if (NInfoGetSpcOid(rinfo) == 0 && NInfoGetDbOid(rinfo) == 0 &&
		NInfoGetRelNumber(rinfo) == 0)
		return false;

	return remote_exists(rinfo, forknum);
}

void
local_branch_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	LWLock	   *lock;

	if (isRedo)
	{
		/*
		 * In WAL replay, this is called to make sure that the fork exists
		 * before it's accessed, so leave existing forks alone.
		 */
		switch (local_fork_lookup(rinfo, forknum, &meta))
		{
			case LB_LOCAL:
				return;
			case LB_NONEXISTENT:
				break;
			case LB_REMOTE:
				if (remote_exists(rinfo, forknum))
					return;
				break;
		}
	}

	/*
	 * Like mdcreate(), make sure that the database's directory exists. The
	 * relation might be the first one in its tablespace.
	 */
	TablespaceCreateDbspace(NInfoGetSpcOid(rinfo), NInfoGetDbOid(rinfo), isRedo);

	/* A newly created fork is empty and doesn't come from the pageserver */
	lock = local_map_lock(rinfo, forknum);
	LWLockAcquire(lock, LW_EXCLUSIVE);
	local_map_write(rinfo, forknum, LOCAL_MAP_EXISTS, 0);
	LWLockRelease(lock);

	set_cached_relsize(rinfo, forknum, 0);
}

/*
 * Called after md.c has removed the data file of the fork.
 */
void
local_branch_unlink(NRelFileInfoBackend rinfob, ForkNumber forknum, bool isRedo)
{
	NRelFileInfo rinfo = InfoFromNInfoB(rinfob);
	LocalForkMeta meta;
	LWLock	   *lock;
	LocalForkState state;

	/* Failures are not errors, we're usually not in a transaction anymore */
	int			elevel = WARNING;

	if (forknum == InvalidForkNumber)
	{
		for (int fork = 0; fork <= MAX_FORKNUM; fork++)
			local_branch_unlink(rinfob, fork, isRedo);
		return;
	}

	lock = local_map_lock(rinfo, forknum);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	if (local_fork_lookup_ext(rinfo, forknum, &meta, &state, elevel))
	{
		if (forknum == MAIN_FORKNUM &&
			(state == LB_REMOTE || (state == LB_LOCAL && meta.remote_backed)))
		{
			/*
			 * The pageserver might have the relation. Leave a tombstone, which
			 * also covers the other forks.
			 */
			(void) local_map_write_ext(rinfo, forknum, 0, 0, elevel);
		}
		else if (state == LB_LOCAL)
			local_map_remove(rinfo, forknum, elevel);
	}

	LWLockRelease(lock);
}

/*
 * Read blocks, from local storage or from the pageserver.
 */
void
local_branch_readv(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
				   void **buffers, BlockNumber nblocks)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	bits8		local[PG_IOV_MAX / 8];
	BlockNumber run_start;

	switch (local_fork_lookup(rinfo, forknum, &meta))
	{
		case LB_REMOTE:
			neon_read_remote(reln, forknum, blkno, buffers, nblocks);
			return;
		case LB_NONEXISTENT:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg(NEON_TAG "could not read block %u of relation %u/%u/%u fork %u: relation does not exist",
							blkno, RelFileInfoFmt(rinfo), forknum)));
			break;
		case LB_LOCAL:
			break;
	}

	local_map_get_local_blocks(rinfo, forknum, &meta, blkno, nblocks, local);
	local_data_read(rinfo, forknum, blkno, buffers, nblocks, local);

	/* Read the rest from the pageserver, a run of consecutive blocks at a time */
	run_start = 0;
	while (run_start < nblocks)
	{
		BlockNumber run_end;

		if (BITMAP_ISSET(local, run_start))
		{
			run_start++;
			continue;
		}
		run_end = run_start + 1;
		while (run_end < nblocks && !BITMAP_ISSET(local, run_end))
			run_end++;

		neon_read_remote(reln, forknum, blkno + run_start, buffers + run_start,
						 run_end - run_start);
		run_start = run_end;
	}
}

void
local_branch_writev(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
					const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	bits8		local[PG_IOV_MAX / 8];
	bits8		first_writes[PG_IOV_MAX / 8];
	bool		any_first_writes = false;

	local_fork_for_write(reln, forknum, &meta, InvalidBlockNumber);

	/* Which blocks from the pageserver are written locally for the first time? */
	local_map_get_local_blocks(rinfo, forknum, &meta, blkno, nblocks, local);
	memset(first_writes, 0, sizeof(first_writes));
	for (BlockNumber i = 0; i < nblocks; i++)
	{
		if (!BITMAP_ISSET(local, i))
		{
			BITMAP_SET(first_writes, i);
			any_first_writes = true;
		}
	}

	/*
	 * Before the first writes are marked in the map, the data must be durable,
	 * see the comment at the top of the file.
	 */
	local_data_write(rinfo, forknum, blkno, buffers, nblocks, any_first_writes);
	if (any_first_writes)
		local_map_mark_local_blocks(rinfo, forknum, blkno, nblocks, first_writes);
	else if (!skipFsync)
		local_data_register_sync(rinfo, forknum);
}

void
local_branch_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
					const void *buffer, bool skipFsync)
{
	local_branch_writev(reln, forknum, blkno, &buffer, 1, skipFsync);

	/*
	 * The blocks between the old end of the relation and blkno, if any, are
	 * holes, which read as zeros.
	 */
	update_cached_relsize(InfoFromSMgrRel(reln), forknum, blkno + 1);
}

void
local_branch_zeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
						int nblocks, bool skipFsync)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;

	local_fork_for_write(reln, forknum, &meta, InvalidBlockNumber);

	/* blkno is at the end of the relation, so these are all local blocks */
	Assert(blkno >= meta.remote_nblocks || !meta.remote_backed);

	local_data_extend(rinfo, forknum, blkno + nblocks);
	if (!skipFsync)
		local_data_register_sync(rinfo, forknum);

	set_cached_relsize(rinfo, forknum, blkno + nblocks);
}

/*
 * Like mdprefetch(), returns false if the fork doesn't exist. WAL replay
 * relies on that.
 */
bool
local_branch_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
					  int nblocks)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	bits8		local[PG_IOV_MAX / 8];

	switch (local_fork_lookup(rinfo, forknum, &meta))
	{
		case LB_REMOTE:
			neon_prefetch_remote(reln, forknum, blkno, nblocks);
			return true;
		case LB_NONEXISTENT:
			return false;
		case LB_LOCAL:
			break;
	}

	if (!meta.remote_backed || blkno >= meta.remote_nblocks)
		return true;

	/* Only prefetch the blocks that come from the pageserver */
	while (nblocks > 0)
	{
		int			iterblocks = Min(nblocks, PG_IOV_MAX);

		local_map_get_local_blocks(rinfo, forknum, &meta, blkno, iterblocks, local);
		for (int i = 0; i < iterblocks; i++)
		{
			if (!BITMAP_ISSET(local, i))
				neon_prefetch_remote(reln, forknum, blkno + i, 1);
		}
		blkno += iterblocks;
		nblocks -= iterblocks;
	}
	return true;
}

BlockNumber
local_branch_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	BlockNumber nblocks;

	if (get_cached_relsize(rinfo, forknum, &nblocks))
		return nblocks;

	switch (local_fork_lookup(rinfo, forknum, &meta))
	{
		case LB_REMOTE:
			nblocks = remote_nblocks(rinfo, forknum);
			break;
		case LB_NONEXISTENT:
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg(NEON_TAG "could not get size of relation %u/%u/%u fork %u: relation does not exist",
							RelFileInfoFmt(rinfo), forknum)));
			break;
		case LB_LOCAL:
			nblocks = local_data_nblocks(rinfo, forknum);
			if (meta.remote_backed)
				nblocks = Max(nblocks, meta.remote_nblocks);
			break;
	}

	update_cached_relsize(rinfo, forknum, nblocks);
	return nblocks;
}

/*
 * This is called in a critical section, so it mustn't allocate memory outside
 * local_branch_truncate_cxt, or do network I/O.
 */
void
local_branch_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber old_blocks,
					  BlockNumber nblocks)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	LocalForkMeta meta;
	MemoryContext oldcxt;

	oldcxt = MemoryContextSwitchTo(local_branch_truncate_cxt);

	/*
	 * If the fork hasn't been modified yet, the size before truncation is its
	 * size on the pageserver.
	 */
	local_fork_for_write(reln, forknum, &meta, old_blocks);

	if (meta.remote_backed && nblocks < meta.remote_nblocks)
		local_map_shrink_remote(rinfo, forknum, nblocks);
	if (local_data_truncate(rinfo, forknum, nblocks))
		local_data_register_sync(rinfo, forknum);

	set_cached_relsize(rinfo, forknum, nblocks);

	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(local_branch_truncate_cxt);
}

void
local_branch_immedsync(SMgrRelation reln, ForkNumber forknum)
{
	char	   *path;
	int			fd;

	fd = local_data_open(InfoFromSMgrRel(reln), forknum, O_RDWR, &path);
	if (fd < 0)
	{
		pfree(path);
		return;
	}
	local_data_fsync(fd, path);
	local_data_close(fd, path);
}

void
local_branch_registersync(SMgrRelation reln, ForkNumber forknum)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);
	struct stat st;
	char	   *path = relpathperm(rinfo, forknum);

	/* Nothing to sync if nothing was written locally */
	if (stat(path, &st) == 0)
		local_data_register_sync(rinfo, forknum);
	pfree(path);
}
