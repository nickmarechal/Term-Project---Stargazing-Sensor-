#include "dsq/dump.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Ring buffer for -n: holds at most this many records (48 MB). */
enum { LAST_N_MAX = 1000000 };

/* A line being built: what fit so far, and how long it wanted to be. */
struct line {
    char *buf;
    size_t cap;
    size_t want;
};

static void add(struct line *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void add(struct line *l, const char *fmt, ...)
{
    va_list ap;
    size_t room = l->want < l->cap ? l->cap - l->want : 0;
    va_start(ap, fmt);
    int w = vsnprintf(room > 0 ? l->buf + l->want : NULL, room, fmt, ap);
    va_end(ap);
    if (w > 0)
        l->want += (size_t)w;
}

static int finish(struct line *l)
{
    if (l->cap > 0 && l->want >= l->cap)
        l->buf[l->cap - 1] = '\0';   /* cut short, still a string */
    return (int)l->want;
}

/* Local time with milliseconds, e.g. 2026-10-28T19:02:11.123-0600. */
static void add_time(struct line *l, uint64_t real_ns)
{
    char t[32], z[8];
    struct tm tm;
    time_t sec = (time_t)(real_ns / 1000000000u);
    unsigned ms = (unsigned)(real_ns % 1000000000u / 1000000u);

    if (real_ns == 0 || localtime_r(&sec, &tm) == NULL
        || strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%S", &tm) == 0
        || strftime(z, sizeof z, "%z", &tm) == 0) {
        add(l, "time-unknown");
        return;
    }
    add(l, "%s.%03u%s", t, ms, z);
}

/* A fixed-point value ×1000 as a decimal: -500 -> "-0.500". */
static void add_milli(struct line *l, int32_t v)
{
    int64_t a = v < 0 ? -(int64_t)v : v;
    add(l, "%s%lld.%03lld", v < 0 ? "-" : "", (long long)(a / 1000), (long long)(a % 1000));
}

static const char *source_name(uint16_t id)
{
    switch (id) {
    case DSM_SRC_SKYD:    return "skyd";
    case DSM_SRC_CLOUDD:  return "cloudd";
    case DSM_SRC_ENVD:    return "envd";
    case DSM_SRC_FUSIOND: return "fusiond";
    case DSM_SRC_DSKYD:   return "dskyd";
    case DSM_SRC_STORED:  return "stored";
    }
    return NULL;
}

static const char *kind_name(uint16_t kind)
{
    switch (kind) {
    case DSM_KIND_SAMPLE:    return "SAMPLE";
    case DSM_KIND_FEATURE:   return "FEATURE";
    case DSM_KIND_STATE:     return "STATE";
    case DSM_KIND_HEARTBEAT: return "HEARTBEAT";
    case DSM_KIND_FAULT:     return "FAULT";
    case DSM_KIND_START:     return "START";
    }
    return NULL;
}

static void add_flags(struct line *l, uint16_t flags)
{
    static const struct { uint16_t bit; const char *name; } names[] = {
        { DSM_FLAG_VALID, "VALID" },
        { DSM_FLAG_STALE, "STALE" },
        { DSM_FLAG_SYNTHETIC, "SYNTHETIC" },
        { DSM_FLAG_CALIBRATING, "CALIBRATING" },
    };
    uint16_t known = 0;
    const char *sep = "";

    add(l, " flags=");
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        known |= names[i].bit;
        if (flags & names[i].bit) {
            add(l, "%s%s", sep, names[i].name);
            sep = ",";
        }
    }
    if (flags & ~known)
        add(l, "%s0x%x", sep, (unsigned)(flags & ~known));
    else if (*sep == '\0')
        add(l, "-");
}

/* The values, in the units of the payload table (docs/record-and-ipc.md).
 * Anything not in the table is shown raw rather than dropped. */
static void add_values(struct line *l, const struct dsm_record *r)
{
    const int32_t *v = r->v;

    if (r->kind == DSM_KIND_START) {
        add(l, " run=%d", v[0]);
    } else if (r->kind == DSM_KIND_FAULT) {
        const char *f = v[0] == DSM_FAULT_READ ? "read"
                      : v[0] == DSM_FAULT_CHIP_ID ? "chip_id" : NULL;
        if (f != NULL)
            add(l, " fault=%s", f);
        else
            add(l, " fault=%d", v[0]);
        add(l, " errno=%d count=%d", v[1], v[2]);
    } else if (r->kind == DSM_KIND_SAMPLE && r->source_id == DSM_SRC_SKYD) {
        add(l, " ch0=%d ch1=%d gain=%dx atime=%dms", v[0], v[1], v[2], v[3]);
    } else if (r->kind == DSM_KIND_SAMPLE && r->source_id == DSM_SRC_CLOUDD) {
        add(l, " sky=");
        add_milli(l, v[0]);
        add(l, "C sensor=");
        add_milli(l, v[1]);
        add(l, "C");
    } else if (r->kind == DSM_KIND_SAMPLE && r->source_id == DSM_SRC_ENVD) {
        add(l, " temp=");
        add_milli(l, v[0]);
        add(l, "C rh=");
        add_milli(l, v[1]);
        add(l, "%% pressure=%dPa", v[2]);
    } else if (r->kind == DSM_KIND_FEATURE) {
        add(l, " feature=%d value=", v[0]);
        add_milli(l, v[1]);
        add(l, " window=%ds n=%d", v[2], v[3]);
    } else if (r->kind == DSM_KIND_STATE) {
        add(l, " state=%d rules=0x%x confidence=", v[0], (unsigned)v[1]);
        add_milli(l, v[2]);
    } else if (r->kind == DSM_KIND_HEARTBEAT) {
        const char *s = source_name((uint16_t)v[0]);
        if (s != NULL)
            add(l, " subject=%s", s);
        else
            add(l, " subject=src%d", v[0]);
        add(l, " rss=%dKiB cpu=%dms events=%d", v[1], v[2], v[3]);
    } else {
        add(l, " v=%d,%d,%d,%d", v[0], v[1], v[2], v[3]);
    }
}

int dump_format_record(char *buf, size_t cap, const struct dsm_record *r)
{
    struct line l = { buf, cap, 0 };
    char src[16], kind[16];
    const char *s = source_name(r->source_id), *k = kind_name(r->kind);

    if (s == NULL)
        snprintf(src, sizeof src, "src%u", r->source_id);
    if (k == NULL)
        snprintf(kind, sizeof kind, "kind%u", r->kind);

    add_time(&l, r->t_real_ns);
    add(&l, " %-7s %-9s seq=%u", s != NULL ? s : src, k != NULL ? k : kind, (unsigned)r->seq);
    add_flags(&l, r->flags);
    add_values(&l, r);
    return finish(&l);
}

int dump_format_header(char *buf, size_t cap, const struct dsm_store_header *h)
{
    struct line l = { buf, cap, 0 };
    add(&l, "header: schema=%u record_size=%u created=", h->schema_ver, h->record_size);
    add_time(&l, h->created_real_ns);
    return finish(&l);
}

int dump_format_summary(char *buf, size_t cap, uint64_t n, uint64_t tail, uint64_t synthetic)
{
    struct line l = { buf, cap, 0 };
    add(&l, "%llu records; torn tail: %llu bytes (not modified)",
        (unsigned long long)n, (unsigned long long)tail);
    if (synthetic > 0)
        add(&l, "; %llu SYNTHETIC", (unsigned long long)synthetic);
    return finish(&l);
}

struct dump_ctx {
    FILE *out;
    struct dsm_record *ring;   /* NULL: print each record as it is visited */
    uint64_t cap;
    uint64_t count;
    uint64_t synthetic;
};

static void print_record(FILE *out, const struct dsm_record *r)
{
    char line[256];
    dump_format_record(line, sizeof line, r);
    fprintf(out, "%s\n", line);
}

static void visit(const struct dsm_record *r, void *p)
{
    struct dump_ctx *c = p;
    if (r->flags & DSM_FLAG_SYNTHETIC)
        c->synthetic++;
    if (c->ring != NULL)
        c->ring[c->count % c->cap] = *r;
    else
        print_record(c->out, r);
    c->count++;
}

int dump_file(const char *path, uint64_t last_n, FILE *out, FILE *err)
{
    struct dump_ctx c = { out, NULL, 0, 0, 0 };
    struct store_recovery rec;
    struct dsm_store_header h;
    char line[256];
    int rc = 1;

    if (last_n > LAST_N_MAX) {
        fprintf(err, "dsq: -n %llu is more than %d\n", (unsigned long long)last_n, LAST_N_MAX);
        return 1;
    }
    if (last_n > 0) {
        c.ring = malloc((size_t)last_n * sizeof *c.ring);
        if (c.ring == NULL) {
            fprintf(err, "dsq: out of memory for -n %llu\n", (unsigned long long)last_n);
            return 1;
        }
        c.cap = last_n;
    }

    /* The header line goes first, but only for a file that is ours:
     * store_scan() below makes the full check and gives the reason. */
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        if (fread(&h, 1, sizeof h, f) == sizeof h
            && memcmp(h.magic, DSM_STORE_MAGIC, sizeof h.magic) == 0) {
            dump_format_header(line, sizeof line, &h);
            fprintf(out, "%s\n", line);
        }
        fclose(f);
    }

    switch (store_scan(path, visit, &c, &rec)) {
    case STORE_OK:
        break;
    case STORE_ERR_FORMAT:
        fprintf(err, "dsq: %s: not a store file\n", path);
        goto done;
    default:
        fprintf(err, "dsq: %s: %s\n", path, strerror(errno));
        goto done;
    }

    if (c.ring != NULL) {
        uint64_t first = c.count > c.cap ? c.count - c.cap : 0;
        for (uint64_t i = first; i < c.count; i++)
            print_record(out, &c.ring[i % c.cap]);
    }
    dump_format_summary(line, sizeof line, c.count, rec.cut_bytes, c.synthetic);
    fprintf(out, "%s\n", line);
    rc = 0;

done:
    free(c.ring);
    return rc;
}
