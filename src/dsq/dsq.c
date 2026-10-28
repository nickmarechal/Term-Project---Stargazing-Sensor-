/* dsq: look at what the Dark Sky Monitor has stored. Portable.
 *
 *   dsq dump <file> [-n N]    every record as text; -n: only the last N
 *
 * The query commands come later (Nick's analysis work). dsq only reads:
 * it never changes a store file, even one with a torn tail. */
#include "dsq/dump.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int usage(void)
{
    fprintf(stderr, "usage: dsq dump <file> [-n N]\n");
    return 1;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    unsigned long long last_n = 0;

    if (argc < 3 || strcmp(argv[1], "dump") != 0)
        return usage();
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            char *end;
            errno = 0;
            last_n = strtoull(argv[++i], &end, 10);
            if (errno != 0 || *end != '\0' || end == argv[i] || last_n == 0
                || argv[i][0] == '-')
                return usage();
        } else if (file == NULL && argv[i][0] != '-') {
            file = argv[i];
        } else {
            return usage();
        }
    }
    if (file == NULL)
        return usage();

    tzset();                   /* record times are shown in local time */
    return dump_file(file, last_n, stdout, stderr);
}
