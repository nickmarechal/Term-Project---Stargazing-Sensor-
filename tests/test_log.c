#include "check.h"
#include "suites.h"
#include "common/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Every test logs into a temporary file, then reads the lines back. */
static FILE *out;

static void start(void)
{
    out = tmpfile();
    CHECK(out != NULL, "tmpfile failed");
    log_init("testd", out);
}

/* Read the next line written so far. Returns "" if there is none. */
static const char *next_line(char *buf, size_t cap)
{
    buf[0] = '\0';
    if (out != NULL && fgets(buf, (int)cap, out) == NULL)
        buf[0] = '\0';
    return buf;
}

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && strcmp(s + n - t, tail) == 0;
}

/* The line is: wall-clock time, monotonic seconds, proc[pid], level,
 * event, then the k=v part. */
static void test_line_format(void)
{
    start();
    log_msg(LVL_WARN, "recover", "kept=%d cut_bytes=%d", 3, 20);
    if (out != NULL)
        rewind(out);

    char line[600], want[100];
    next_line(line, sizeof line);
    snprintf(want, sizeof want, " testd[%ld] WARN recover kept=3 cut_bytes=20\n",
             (long)getpid());
    CHECK(ends_with(line, want), "line is \"%s\"", line);

    /* 2026-10-28T19:02:11-0600: positions of the separators. */
    CHECK(strlen(line) > 25 && line[4] == '-' && line[7] == '-' && line[10] == 'T'
          && line[13] == ':' && line[16] == ':'
          && (line[19] == '-' || line[19] == '+') && line[24] == ' ',
          "time isn't ISO 8601 with a zone: \"%s\"", line);

    /* Then monotonic seconds with 3 decimals. */
    char *end = NULL;
    double mono = strtod(line + 25, &end);
    CHECK(mono > 0 && end != NULL && *end == ' ' && end[-4] == '.',
          "monotonic seconds missing: \"%s\"", line);
    fclose(out);
}

static void test_event_only_and_levels(void)
{
    start();
    log_msg(LVL_INFO, "start", NULL);
    log_msg(LVL_ERROR, "write_fail", "errno=%d", 28);
    if (out != NULL)
        rewind(out);

    char line[600];
    CHECK(ends_with(next_line(line, sizeof line), "] INFO start\n"),
          "first line is \"%s\"", line);
    CHECK(ends_with(next_line(line, sizeof line), "] ERROR write_fail errno=28\n"),
          "second line is \"%s\"", line);
    CHECK(next_line(line, sizeof line)[0] == '\0', "a third line appeared: \"%s\"", line);
    fclose(out);
}

/* A message bigger than the line buffer is cut short, not overflowed,
 * and the line still ends with a newline. */
static void test_long_line_cut_short(void)
{
    char big[2000];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    start();
    log_msg(LVL_INFO, "big", "v=%s", big);
    if (out != NULL)
        rewind(out);

    char line[3000];
    next_line(line, sizeof line);
    size_t n = strlen(line);
    CHECK(n > 100 && n < 600, "long line is %zu bytes", n);
    CHECK(n > 0 && line[n - 1] == '\n', "long line doesn't end with a newline");
    fclose(out);
}

void test_log(void)
{
    test_line_format();
    test_event_only_and_levels();
    test_long_line_cut_short();
    log_init("testd", NULL);   /* don't leave a closed FILE behind */
}
