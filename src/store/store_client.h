#ifndef DSM_STORE_CLIENT_H
#define DSM_STORE_CLIENT_H

#include "common/record.h"

#include <stdint.h>

/* The sending half of the stored protocol, used by every process that
 * produces records (the sensor programs now; fusiond and dskyd at M4).
 *
 * The protocol (docs/m3-plan.md): AF_UNIX SOCK_SEQPACKET, one sealed
 * 48-byte struct dsm_record per message, clients only send, stored never
 * replies. Linux only: macOS does not support SOCK_SEQPACKET.
 *
 * This header is the agreement between the sensor side and the storage
 * side. Change it only when both partners agree. */

/* Opaque: only store_client.c can see inside. */
struct store_client;

/* Remembers the socket path. Does not connect yet, so it works whether
 * or not stored is running. NULL only if out of memory or the path is
 * too long for a sockaddr_un (errno says which). */
struct store_client *store_client_new(const char *socket_path);

/* Sends one sealed record as one message. Never blocks: if stored is
 * down, slow or gone, the record is dropped and counted, and the caller
 * keeps sampling (the seq gap shows the loss). Reconnects on its own,
 * at most one attempt per second. Returns 0 if sent, -1 if dropped. */
int store_client_send(struct store_client *c, const struct dsm_record *r);

/* Records dropped since store_client_new(). */
uint64_t store_client_dropped(const struct store_client *c);

/* Closes the connection and frees. NULL-safe. */
void store_client_free(struct store_client *c);

#endif
