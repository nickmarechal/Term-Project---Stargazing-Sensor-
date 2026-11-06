#ifndef DSM_ANALYSIS_FEATURES_H
#define DSM_ANALYSIS_FEATURES_H

#include "common/record.h"

#include <stdbool.h>
#include <stdint.h>

/* Turns the raw SAMPLE records into the numbers the classifier decides on.
 * Portable, with no clock and no I/O: the caller feeds in records and passes
 * the current time, so tests and replay work exactly like live data.
 * One allocation at features_new() and none after. Owner: Nick. */

#define FEAT_WIN_LEVEL_S   60      /* sky_rel, d_t, d_t_corr */
#define FEAT_WIN_SIGMA_S   1800    /* sky_sigma, d_t_sigma */
#define FEAT_WIN_DEW_S     300     /* dew_margin */
#define FEAT_WIN_PRESS_S   10800   /* press_trend */

enum feat_sensor {
    FEAT_SENSOR_SKY,      /* skyd */
    FEAT_SENSOR_CLOUD,    /* cloudd */
    FEAT_SENSOR_ENV,      /* envd */
    FEAT_SENSOR_COUNT
};

/* One computed number. When ok is false there wasn't enough data in the
 * window, value means nothing and must not be used. */
struct feat_value {
    bool ok;
    double value;
    uint32_t samples;   /* samples behind this value */
};

struct feat_health {
    bool seen;             /* at least one record from this sensor, ever */
    bool stale;            /* no VALID sample for 3 of its sample periods */
    double completeness;   /* fraction of the expected samples, 0 to 1, over
                              the last FEAT_WIN_SIGMA_S */
    uint64_t accepted;     /* VALID samples used */
    uint64_t rejected;     /* SAMPLEs without VALID: counted, never used */
};

struct feat_snapshot {
    struct feat_value sky_rel;       /* brightness in magnitudes, relative to
                                        an arbitrary zero; larger = darker.
                                        Never an absolute mag/arcsec2. */
    struct feat_value sky_sigma;     /* spread of sky_rel over time, mag */
    struct feat_value d_t;           /* air temp minus sky temp, deg C;
                                        larger = clearer */
    struct feat_value d_t_corr;      /* d_t with the humidity correction */
    struct feat_value d_t_sigma;     /* spread of d_t over time, deg C */
    struct feat_value dew_margin;    /* air temp minus dew point, deg C */
    struct feat_value press_trend;   /* pressure change, Pa per hour */
    bool d_t_corr_fitted;            /* false: no correction fitted from real
                                        data yet, so d_t_corr equals d_t */
    bool synthetic;                  /* any input in the windows was SYNTHETIC;
                                        anything built on this must say so */
    struct feat_health health[FEAT_SENSOR_COUNT];
};

struct features;

/* NULL if out of memory. */
struct features *features_new(void);
void features_free(struct features *f);

/* Takes one record. Only SAMPLEs from skyd, cloudd and envd are used; anything
 * else is ignored, so a caller can pass everything it reads. A t_mono_ns lower
 * than the last one means the device rebooted, and the windows start over. */
void features_push(struct features *f, const struct dsm_record *r);

/* The numbers as of now_mono_ns (CLOCK_MONOTONIC, or the last record's t_mono_ns
 * during replay). Doesn't change the state, so it is safe to call at any rate. */
void features_get(const struct features *f, uint64_t now_mono_ns, struct feat_snapshot *out);

#endif
