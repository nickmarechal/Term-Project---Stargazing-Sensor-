#ifndef DSM_DUMP_H
#define DSM_DUMP_H

#include "common/record.h"
#include "store/store.h"

#include <stdint.h>
#include <stdio.h>

/* dsq dump: a store file as text, one line per record, in units from the
 * payload table in docs/record-and-ipc.md. Portable (no sockets). Reads the
 * file through store_scan(), so it never changes it, even a torn tail, and
 * is safe while stored is writing. */

/* Each formatter writes one line without a newline into buf and returns
 * the length it wanted, like snprintf: a longer line is cut short. */

/* "header: schema=1 record_size=48 created=<local time>" */
int dump_format_header(char *buf, size_t cap, const struct dsm_store_header *h);

/* "<local time> <source> <kind> seq=<n> flags=<..> <values>". Times are
 * local, from t_real_ns; SYNTHETIC is spelled out in flags. */
int dump_format_record(char *buf, size_t cap, const struct dsm_record *r);

/* "<n> records; torn tail: <k> bytes (not modified)", plus
 * "; <s> SYNTHETIC" when any record is synthetic. */
int dump_format_summary(char *buf, size_t cap, uint64_t n, uint64_t tail, uint64_t synthetic);

/* The whole dump of path to out. last_n > 0 shows only the last last_n
 * records (the summary still counts all). Returns 0, or 1 after printing
 * the reason to err. */
int dump_file(const char *path, uint64_t last_n, FILE *out, FILE *err);

#endif
