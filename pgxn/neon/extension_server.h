/*-------------------------------------------------------------------------
 *
 * extension_server.h
 *	  Request compute_ctl to download extension files.
 *
 *-------------------------------------------------------------------------
 */

#ifndef EXTENSION_SERVER_H
#define EXTENSION_SERVER_H

extern int	hadron_extension_server_port;

void pg_init_extension_server(void);

#endif							/* EXTENSION_SERVER_H */
