/*-------------------------------------------------------------------------
 *
 * local_branch.h
 *	  Local branches: computes that keep their writes on local disk.
 *
 * See local_branch.c for an overview.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCAL_BRANCH_H
#define LOCAL_BRANCH_H

#include "neon_pgversioncompat.h"

#include "access/xlogdefs.h"
#include "common/relpath.h"
#include "storage/block.h"
#include "storage/smgr.h"
#include RELFILEINFO_HDR

/* Is this compute running a local branch? Set at postmaster startup. */
extern bool neon_local_branch;

/*
 * The LSN of the pageserver timeline that the local branch starts from. All
 * pages that the branch hasn't modified are read at this LSN.
 */
extern XLogRecPtr local_branch_lsn;

extern void pg_init_local_branch(void);
extern void LocalBranchShmemRequest(void);
extern void LocalBranchShmemInit(void);
extern void local_branch_init_backend(void);

/* smgr functions for permanent relations, used instead of the regular ones */
extern bool local_branch_exists(SMgrRelation reln, ForkNumber forknum);
extern void local_branch_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern void local_branch_unlink(NRelFileInfoBackend rinfo, ForkNumber forknum, bool isRedo);
extern void local_branch_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
								const void *buffer, bool skipFsync);
extern void local_branch_zeroextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
									int nblocks, bool skipFsync);
extern bool local_branch_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
								  int nblocks);
extern void local_branch_readv(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
							   void **buffers, BlockNumber nblocks);
extern void local_branch_writev(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
								const void **buffers, BlockNumber nblocks, bool skipFsync);
extern BlockNumber local_branch_nblocks(SMgrRelation reln, ForkNumber forknum);
extern void local_branch_truncate(SMgrRelation reln, ForkNumber forknum,
								  BlockNumber old_blocks, BlockNumber nblocks);
extern void local_branch_immedsync(SMgrRelation reln, ForkNumber forknum);
extern void local_branch_registersync(SMgrRelation reln, ForkNumber forknum);

/*
 * Is a write to a relation of unknown persistence (see pagestore_smgr.c) a
 * write to a permanent relation?
 */
extern bool local_branch_is_permanent(SMgrRelation reln, ForkNumber forknum);

#endif							/* LOCAL_BRANCH_H */
