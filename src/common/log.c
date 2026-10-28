#include "common/log.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { LINE_MAX_BYTES = 512 };

static const char *log_proc = "?";
static FILE *log_out;          /* NULL means stderr */

void log_init(const char *proc, FILE *out)
{
    log_proc = proc;
    log_out = out;
    tzset();                   /* so localtime_r uses this machine's time zone */
}

static const char *level_name(enum log_level lvl)
{
    switch (lvl) {
    case LVL_INFO:  return "INFO";
    case LVL_WARN:  return "WARN";
    case LVL_ERROR: return "ERROR";
    }
    return "?";
}

/* Add to the line. If it doesn't fit, it is cut short. One byte is always
 * left free for the '\n'. */
static void vadd(char *line, size_t *len, const char *fmt, va_list ap)
{
    size_t room = LINE_MAX_BYTES - 1 - *len;   /* includes vsnprintf's '\0' */
    int w = vsnprintf(line + *len, room, fmt, ap);
    if (w < 0)
        return;
    *len += (size_t)w < room ? (size_t)w : room - 1;
}

static void add(char *line, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vadd(line, len, fmt, ap);
    va_end(ap);
}

void log_msg(enum log_level lvl, const char *event, const char *fmt, ...)
{
    char line[LINE_MAX_BYTES];
    size_t len = 0;
    struct timespec real = {0, 0}, mono = {0, 0};
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &real);
    clock_gettime(CLOCK_MONOTONIC, &mono);

    if (localtime_r(&real.tv_sec, &tm) != NULL)
        len = strftime(line, sizeof line - 1, "%Y-%m-%dT%H:%M:%S%z", &tm);
    if (len == 0)
        add(line, &len, "0000-00-00T00:00:00+0000");   /* time unknown */

    add(line, &len, " %lld.%03ld %s[%ld] %s %s",
        (long long)mono.tv_sec, mono.tv_nsec / 1000000L,
        log_proc, (long)getpid(), level_name(lvl), event);

    if (fmt != NULL) {
        va_list ap;
        add(line, &len, " ");
        va_start(ap, fmt);
        vadd(line, &len, fmt, ap);
        va_end(ap);
    }
    line[len++] = '\n';

    /* The whole line is built first and written with one call, so it isn't
     * split into pieces the way several fprintf calls could split it. */
    FILE *out = log_out != NULL ? log_out : stderr;
    fwrite(line, 1, len, out);
    fflush(out);
}
