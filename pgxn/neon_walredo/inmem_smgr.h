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
extern SmgrId smgr_register_inmem(void);
extern void smgr_reset_inmem(void);
#endif
extern const f_smgr *smgr_inmem(ProcNumber backend, NRelFileInfo rinfo);
extern void smgr_init_inmem(void);

#endif /* INMEM_SMGR_H */
