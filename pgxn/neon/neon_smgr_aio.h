/*-------------------------------------------------------------------------
 *
 * neon_smgr_aio.h
 *	  Glue between Neon's storage managers and the AIO subsystem of v18+
 *
 * Since v18, the buffer manager reads all blocks through smgrstartreadv(),
 * which is expected to stage an asynchronous read of a file, using a
 * PgAioHandle. Neon's storage managers don't read files: the compute gets
 * its pages from the pageserver, the local file cache or the prefetch
 * buffers, and the WAL redo process from memory. None of that is something
 * the AIO subsystem could execute, and there is no file descriptor to give
 * it. So our smgr_startreadv() reads the pages synchronously, and then uses
 * neon_aio_complete_readv() to complete the handle as an IO that has already
 * been performed.
 *
 *-------------------------------------------------------------------------
 */
#ifndef NEON_SMGR_AIO_H
#define NEON_SMGR_AIO_H

#if PG_MAJORVERSION_NUM >= 18

#include "miscadmin.h"
#include "storage/aio.h"
#include "storage/aio_internal.h"
#include "storage/smgr.h"

/*
 * Complete the AIO handle of an smgr_startreadv() call whose 'nblocks' blocks
 * the caller has already read into the target buffers.
 *
 * This takes the handle through the same state transitions as an IO that
 * the AIO subsystem executes synchronously (see pgaio_io_stage() and
 * pgaio_io_perform_synchronously()), with 'nblocks' as the result of the
 * IO. As we don't register md.c's completion callback, which would convert
 * a number of bytes into a number of blocks, the buffer manager's callback
 * takes it as the number of blocks read, verifies the pages and marks the
 * buffers valid.
 *
 * This doesn't make any system calls, so it also works in the WAL redo
 * process under seccomp.
 */
static inline void
neon_aio_complete_readv(PgAioHandle *ioh, SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks)
{
	/*
	 * The IO is performed by the time anyone could wait for it, so they must
	 * not wait for the IO method (e.g. io_uring) to complete it.
	 */
	pgaio_io_set_flag(ioh, PGAIO_HF_SYNCHRONOUS);
	pgaio_io_set_target_smgr(ioh, reln, forknum, blocknum, nblocks, false);

	/* the checks of pgaio_io_before_start() */
	Assert(ioh->state == PGAIO_HS_HANDED_OUT);
	Assert(pgaio_my_backend->handed_out_io == ioh);
	Assert(ioh->op == PGAIO_OP_INVALID);

	HOLD_INTERRUPTS();

	/* pgaio_io_start_readv(), without a file to read */
	ioh->op_data.read.fd = -1;
	ioh->op_data.read.offset = (uint64) blocknum * BLCKSZ;
	ioh->op_data.read.iov_length = 0;

	/* pgaio_io_stage() */
	ioh->op = PGAIO_OP_READV;
	ioh->result = 0;
	pg_write_barrier();
	ioh->state = PGAIO_HS_DEFINED;
	pgaio_my_backend->handed_out_io = NULL;
	pgaio_io_call_stage(ioh);
	pg_write_barrier();
	ioh->state = PGAIO_HS_STAGED;

	/* the synchronous execution path of pgaio_io_stage() */
	pgaio_io_prepare_submit(ioh);
	START_CRIT_SECTION();
	pgaio_io_process_completion(ioh, nblocks);
	END_CRIT_SECTION();

	RESUME_INTERRUPTS();
}

#endif							/* PG_MAJORVERSION_NUM >= 18 */

#endif							/* NEON_SMGR_AIO_H */
