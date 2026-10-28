#ifndef DSM_LOG_H
#define DSM_LOG_H

#include <stdio.h>

/* Not LOG_*: <syslog.h> already uses those names. */
enum log_level {
    LVL_INFO,
    LVL_WARN,
    LVL_ERROR,
};

/* Call once at start. proc is the program's name. out is where lines go:
 * stderr in the daemons (journald collects it under systemd). */
void log_init(const char *proc, FILE *out);

/* Writes one line, in the format DESIGN.md promises:
 *
 *   2026-10-28T19:02:11-0600 1234.567 stored[812] WARN recover kept=118 cut_bytes=31
 *   <wall-clock time>        <mono s> <proc>[pid] LEVEL event  k=v ...
 *
 * fmt is the k=v part, printf-style, or NULL for none. A line longer than
 * the buffer is cut short but still ends with a newline. */
void log_msg(enum log_level lvl, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
