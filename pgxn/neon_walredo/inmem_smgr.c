/*-------------------------------------------------------------------------
 *
 * inmem_smgr.c
 *
 * This is an implementation of the SMGR interface, used in the WAL redo
 * process. It has no persistent storage, the pages that are written out
 * are kept in a small number of in-memory buffers.
 *
 * Normally, replaying a WAL record only needs to access a handful of
 * buffers, which fit in the normal buffer cache, so this is just for
 * "overflow" storage when the buffer cache is not large enough.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#if PG_MAJORVERSION_NUM >= 18
#include "storage/aio.h"
#endif

#include "../neon/neon_pgversioncompat.h"

#include "access/xlog.h"
#include "storage/block.h"
#include "storage/buf_internals.h"
#include RELFILEINFO_HDR
#include "storage/smgr.h"

#if PG_VERSION_NUM >= 150000
#include "access/xlogutils.h"
#endif

#include "inmem_smgr.h"

/* Size of the in-memory smgr: XLR_MAX_BLOCK_ID is 32, so assume that 64 will be enough */
#define MAX_PAGES 64

/* If more than WARN_PAGES are used, print a warning in the log */
#define WARN_PAGES 32

static BufferTag page_tag[MAX_PAGES];
static char page_body[MAX_PAGES][BLCKSZ];
static int	used_pages;

static int
locate_page(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);

	/* We only hold a small number of pages, so linear search */
	for (int i = 0; i < used_pages; i++)
	{
		if (RelFileInfoEquals(rinfo, BufTagGetNRelFileInfo(page_tag[i]))
			&& forknum == page_tag[i].forkNum
			&& blkno == page_tag[i].blockNum)
		{
			return i;
		}
	}
	return -1;
}


/* neon wal-redo storage manager functionality */
static void inmem_init(void);

#if PG_MAJORVERSION_NUM >= 18
/*
 * Reset the in-memory smgr between WAL records.
 *
 * walredo used to call smgrinit() for this. In PG17 that was just
 * (*smgr_init_hook)(), but PG18's smgrinit() also does
 * on_proc_exit(smgrshutdown) (smgr.c:200), so calling it per record burns an
 * exit slot each time and the process dies with "out of on_proc_exit slots"
 * (MAX_ON_EXITS is 20) on the twentieth record. Call the smgr's own init
 * directly instead - resetting state is all walredo wanted.
 */
void
smgr_reset_inmem(void)
{
	inmem_init();
}
#endif
static void inmem_open(SMgrRelation reln);
static void inmem_close(SMgrRelation reln, ForkNumber forknum);
static void inmem_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
static bool inmem_exists(SMgrRelation reln, ForkNumber forknum);
static void inmem_unlink(NRelFileInfoBackend rinfo, ForkNumber forknum, bool isRedo);
#if PG_MAJORVERSION_NUM >= 17
static bool inmem_prefetch(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum, int nblocks);
#else
static bool inmem_prefetch(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber blocknum);
#endif
#if PG_MAJORVERSION_NUM < 16
static void inmem_extend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, char *buffer, bool skipFsync);
static void inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					   char *buffer);
static void inmem_write(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, char *buffer, bool skipFsync);
#else
static void inmem_extend(SMgrRelation reln, ForkNumber forknum,
						 BlockNumber blocknum, const void *buffer, bool skipFsync);
static void inmem_zeroextend(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blocknum, int nblocks, bool skipFsync);
static void inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
					   void *buffer);
static void inmem_write(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, const void *buffer, bool skipFsync);
#endif
static void inmem_writeback(SMgrRelation reln, ForkNumber forknum,
							BlockNumber blocknum, BlockNumber nblocks);
static BlockNumber inmem_nblocks(SMgrRelation reln, ForkNumber forknum);
static void inmem_truncate(SMgrRelation reln, ForkNumber forknum,
						   BlockNumber old_blocks, BlockNumber nblocks);
static void inmem_immedsync(SMgrRelation reln, ForkNumber forknum);
#if PG_MAJORVERSION_NUM >= 17
static void inmem_registersync(SMgrRelation reln, ForkNumber forknum);
#endif

/*
 *	inmem_init() -- Initialize private state
 */
static void
inmem_init(void)
{
	used_pages = 0;
}

/*
 *	inmem_exists() -- Does the physical file exist?
 */
static bool
inmem_exists(SMgrRelation reln, ForkNumber forknum)
{
	NRelFileInfo rinfo = InfoFromSMgrRel(reln);

	for (int i = 0; i < used_pages; i++)
	{
		if (RelFileInfoEquals(rinfo, BufTagGetNRelFileInfo(page_tag[i]))
			&& forknum == page_tag[i].forkNum)
		{
			return true;
		}
	}
	return false;
}

/*
 *	inmem_create() -- Create a new relation on neon storage
 *
 * If isRedo is true, it's okay for the relation to exist already.
 */
static void
inmem_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
}

/*
 *	inmem_unlink() -- Unlink a relation.
 */
static void
inmem_unlink(NRelFileInfoBackend rinfo, ForkNumber forknum, bool isRedo)
{
}

/*
 *	inmem_extend() -- Add a block to the specified relation.
 *
 *		The semantics are nearly the same as mdwrite(): write at the
 *		specified position.  However, this is to be used for the case of
 *		extending a relation (i.e., blocknum is at or beyond the current
 *		EOF).  Note that we assume writing a block beyond current EOF
 *		causes intervening file space to become filled with zeroes.
 */
static void
inmem_extend(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
#if PG_MAJORVERSION_NUM < 16
			 char *buffer, bool skipFsync)
#else
			 const void *buffer, bool skipFsync)
#endif
{
	/* same as smgwrite() for us */
	inmem_write(reln, forknum, blkno, buffer, skipFsync);
}

#if PG_MAJORVERSION_NUM >= 16
static void
inmem_zeroextend(SMgrRelation reln, ForkNumber forknum,
				 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	/* Do nothing: inmem_read will return zero page in any case */
}
#endif

/*
 *  inmem_open() -- Initialize newly-opened relation.
 */
static void
inmem_open(SMgrRelation reln)
{
}

/*
 *	inmem_close() -- Close the specified relation, if it isn't closed already.
 */
static void
inmem_close(SMgrRelation reln, ForkNumber forknum)
{
}

#if PG_MAJORVERSION_NUM >= 17
static bool
inmem_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			   int nblocks)
{
	return true;
}
#else
/*
 *	inmem_prefetch() -- Initiate asynchronous read of the specified block of a relation
 */
static bool
inmem_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return true;
}
#endif

/*
 * inmem_writeback() -- Tell the kernel to write pages back to storage.
 */
static void
inmem_writeback(SMgrRelation reln, ForkNumber forknum,
				BlockNumber blocknum, BlockNumber nblocks)
{
}

/*
 *	inmem_read() -- Read the specified block from a relation.
 */
#if PG_MAJORVERSION_NUM < 16
static void
inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
		   char *buffer)
#else
static void
inmem_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
		   void *buffer)
#endif
{
	int			pg;

	pg = locate_page(reln, forknum, blkno);
	if (pg < 0)
		memset(buffer, 0, BLCKSZ);
	else
		memcpy(buffer, page_body[pg], BLCKSZ);
}

#if PG_MAJORVERSION_NUM >= 17
static void
inmem_readv(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			void **buffers, BlockNumber nblocks)
{
	for (int i = 0; i < nblocks; i++)
	{
		inmem_read(reln, forknum, blkno, buffers[i]);
	}
}
#endif

/*
 *	inmem_write() -- Write the supplied block at the appropriate location.
 *
 *		This is to be used only for updating already-existing blocks of a
 *		relation (ie, those before the current EOF).  To extend a relation,
 *		use mdextend().
 */
static void
inmem_write(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
#if PG_MAJORVERSION_NUM < 16
			char *buffer, bool skipFsync)
#else
			const void *buffer, bool skipFsync)
#endif
{
	int			pg;

	pg = locate_page(reln, forknum, blocknum);
	if (pg < 0)
	{
		/*
		 * We assume the buffer cache is large enough to hold all the buffers
		 * needed for most operations. Overflowing to this "in-mem smgr" in
		 * rare cases is OK. But if we find that we're using more than
		 * WARN_PAGES, print a warning so that we get alerted and get to
		 * investigate why we're accessing so many buffers.
		 */
		if (used_pages >= WARN_PAGES)
			ereport(WARNING, (errmsg("inmem_write() called for %u/%u/%u.%u blk %u: used_pages %u",
								   RelFileInfoFmt(InfoFromSMgrRel(reln)),
								   forknum,
								   blocknum,
								   used_pages), errbacktrace()));
		if (used_pages == MAX_PAGES)
			elog(ERROR, "Inmem storage overflow");

		pg = used_pages;
		used_pages++;

		InitBufferTag(&page_tag[pg], &InfoFromSMgrRel(reln), forknum, blocknum);
	}
	else
	{
		elog(DEBUG1, "inmem_write() called for %u/%u/%u.%u blk %u: found at %u",
			 RelFileInfoFmt(InfoFromSMgrRel(reln)),
			 forknum,
			 blocknum,
			 used_pages);
	}
	memcpy(page_body[pg], buffer, BLCKSZ);
}

#if PG_MAJORVERSION_NUM >= 17
static void
inmem_writev(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	for (int i = 0; i < nblocks; i++)
	{
		inmem_write(reln, forknum, blkno, buffers[i], skipFsync);
	}
}
#endif

/*
 *	inmem_nblocks() -- Get the number of blocks stored in a relation.
 */
static BlockNumber
inmem_nblocks(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * It's not clear why a WAL redo function would call smgrnblocks(). During
	 * recovery, at least before reaching consistency, the size of a relation
	 * could be arbitrarily small, if it was truncated after the record being
	 * replayed, or arbitrarily large if it was extended afterwards. But one
	 * place where it's called is in XLogReadBufferExtended(): it extends the
	 * relation, if it's smaller than the requested page. That's a waste of
	 * time in the WAL redo process. Pretend that all relations are maximally
	 * sized to avoid it.
	 */
	return MaxBlockNumber;
}

/*
 *	inmem_truncate() -- Truncate relation to specified number of blocks.
 */
static void
inmem_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber old_blocks, BlockNumber nblocks)
{
}

/*
 *	inmem_immedsync() -- Immediately sync a relation to stable storage.
 */
static void
inmem_immedsync(SMgrRelation reln, ForkNumber forknum)
{
}

#if PG_MAJORVERSION_NUM >= 17
static void
inmem_registersync(SMgrRelation reln, ForkNumber forknum)
{
}
#endif

#if PG_MAJORVERSION_NUM >= 18
/*
 * In the walredo process this smgr stands in for all storage, so it claims every
 * relation - which is what the old smgr_inmem() did unconditionally. Registering
 * it after md means it is consulted first and always wins.
 */
static bool
inmem_owns(RelFileLocator rlocator, ProcNumber backend, char relpersistence)
{
	return true;
}

/* Everything is in memory; the only cap is what inmem_readv can describe. */
static uint32
inmem_maxcombine(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
	return PG_IOV_MAX;
}

/*
 * There is no real IO to start: copy the pages out and complete the handle
 * immediately, so bufmgr's completion callbacks still run. See the equivalent in
 * pgxn/neon/pagestore_smgr.c for why staging happens before the read.
 */
static void
inmem_startreadv(PgAioHandle *ioh,
				 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				 void **buffers, BlockNumber nblocks)
{
	pgaio_io_set_target_smgr(ioh, reln, forknum, blocknum, nblocks, false);
	pgaio_io_stage_external(ioh, PGAIO_OP_READV);

	PG_TRY();
	{
		inmem_readv(reln, forknum, blocknum, buffers, nblocks);
	}
	PG_CATCH();
	{
		pgaio_io_complete_external(ioh, 0);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pgaio_io_complete_external(ioh, nblocks);
}

/* Unreachable: inmem_startreadv never stages an fd-based op. Fail loudly. */
static int
inmem_fd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off)
{
	elog(ERROR, "inmem_fd called: the walredo in-memory smgr has no file descriptor");
	return -1;
}
#endif							/* PG_MAJORVERSION_NUM >= 18 */

static const struct f_smgr inmem_smgr =
{
#if PG_MAJORVERSION_NUM >= 18
	.smgr_name = "inmem",
#endif
	.smgr_init = inmem_init,
	.smgr_shutdown = NULL,
	.smgr_open = inmem_open,
	.smgr_close = inmem_close,
	.smgr_create = inmem_create,
	.smgr_exists = inmem_exists,
	.smgr_unlink = inmem_unlink,
	.smgr_extend = inmem_extend,
#if PG_MAJORVERSION_NUM >= 16
	.smgr_zeroextend = inmem_zeroextend,
#endif
#if PG_MAJORVERSION_NUM >= 17
	.smgr_prefetch = inmem_prefetch,
	.smgr_readv = inmem_readv,
	.smgr_writev = inmem_writev,
#else
	.smgr_prefetch = inmem_prefetch,
	.smgr_read = inmem_read,
	.smgr_write = inmem_write,
#endif
	.smgr_writeback = inmem_writeback,
	.smgr_nblocks = inmem_nblocks,
	.smgr_truncate = inmem_truncate,
	.smgr_immedsync = inmem_immedsync,

#if PG_MAJORVERSION_NUM >= 17
	.smgr_registersync = inmem_registersync,
#endif

#if PG_MAJORVERSION_NUM >= 18
	/* PG18: the four entries that used to be here are standalone hooks now,
	 * and walredo installs none of them. */
	.smgr_maxcombine = inmem_maxcombine,
	.smgr_startreadv = inmem_startreadv,
	.smgr_fd = inmem_fd,
	.smgr_owns = inmem_owns,
#else
	.smgr_start_unlogged_build = NULL,
	.smgr_finish_unlogged_build_phase_1 = NULL,
	.smgr_end_unlogged_build = NULL,
	.smgr_read_slru_segment = NULL,
#endif
};

#if PG_MAJORVERSION_NUM >= 18
SmgrId
smgr_register_inmem(void)
{
	return smgrregister(&inmem_smgr);
}
#endif

const f_smgr *
smgr_inmem(ProcNumber backend, NRelFileInfo rinfo)
{
	Assert(InRecovery);
	// // What does this code do?
	// if (backend != INVALID_PROC_NUMBER)
	// 	return smgr_standard(backend, rinfo);
	// else
	return &inmem_smgr;
}

void
smgr_init_inmem()
{
	inmem_init();
}
