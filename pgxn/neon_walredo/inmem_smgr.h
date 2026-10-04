/*-------------------------------------------------------------------------
 *
 * inmem_smgr.h
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef INMEM_SMGR_H
#define INMEM_SMGR_H

#if PG_MAJORVERSION_NUM >= 18
extern void smgr_register_inmem(void);
#else
extern const f_smgr *smgr_inmem(ProcNumber backend, NRelFileInfo rinfo);
#endif
extern void smgr_init_inmem(void);

#endif /* INMEM_SMGR_H */
