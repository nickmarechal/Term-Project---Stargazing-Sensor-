#ifndef DSM_ANALYSIS_VERDICT_H
#define DSM_ANALYSIS_VERDICT_H

#include "analysis/ephemeris.h"
#include "analysis/features.h"

#include <stdbool.h>
#include <stdint.h>

/* The numbers both partners share, and the classifier's signature. Every enum
 * value here is written to disk (docs/record-and-ipc.md, Payloads), and old
 * files keep their meaning only if they never change: add new ones at the end.
 * tests/test_verdict.c pins them. classify() is written by Liam. */

/* v[0] of a STATE record. 0 is never used. */
enum dsm_state {
    DSM_STATE_BOOT     = 1,
    DSM_STATE_DAYLIGHT = 2,
    DSM_STATE_TWILIGHT = 3,
    DSM_STATE_UNKNOWN  = 4,
    DSM_STATE_POOR     = 5,
    DSM_STATE_MARGINAL = 6,
    DSM_STATE_GOOD     = 7,
};

/* v[1] of a STATE record: every rule that fired, so a verdict can always be
 * explained afterward. */
enum dsm_rule {
    DSM_RULE_SUN_UP            = 1u << 0,
    DSM_RULE_TWILIGHT          = 1u << 1,
    DSM_RULE_SENSOR_MISSING    = 1u << 2,
    DSM_RULE_LOW_DATA          = 1u << 3,
    DSM_RULE_BASELINE_IMMATURE = 1u << 4,
    DSM_RULE_CLOCK_UNSYNC      = 1u << 5,
    DSM_RULE_OVERCAST          = 1u << 6,
    DSM_RULE_DEW               = 1u << 7,
    DSM_RULE_BROKEN_CLOUD      = 1u << 8,
    DSM_RULE_UNSTABLE_SKY      = 1u << 9,
    DSM_RULE_MOONLIGHT         = 1u << 10,
    DSM_RULE_NOT_DARK_ENOUGH   = 1u << 11,
};

/* v[0] of a FEATURE record; v[1] is the value x 1000. */
enum dsm_feature {
    DSM_FEAT_SKY_REL     = 1,
    DSM_FEAT_SKY_SIGMA   = 2,
    DSM_FEAT_DT          = 3,
    DSM_FEAT_DT_CORR     = 4,
    DSM_FEAT_DT_SIGMA    = 5,
    DSM_FEAT_DEW_MARGIN  = 6,
    DSM_FEAT_PRESS_TREND = 7,
    DSM_FEAT_SUN_ALT     = 8,
    DSM_FEAT_MOON_ALT    = 9,
    DSM_FEAT_MOON_ILLUM  = 10,
};

/* What the site's past nights say "good" looks like. nights == 0 means there
 * is no baseline yet. Where this comes from is still open (docs/m4-plan.md). */
struct site_baseline {
    uint32_t nights;       /* nights behind good_sky_rel */
    double good_sky_rel;   /* sky_rel at or above this counts as the site's
                              top band */
};

struct verdict {
    enum dsm_state state;
    uint32_t rules;          /* enum dsm_rule bits */
    double confidence;       /* a score from a published formula, 0 to 1. Not
                                a probability (docs/design-review.md, sec. 14) */
    bool synthetic;          /* the inputs were; set DSM_FLAG_SYNTHETIC on the
                                STATE record */
};

/* The verdict for this one moment, from the veto rules. No memory of earlier
 * moments: minimum dwell time and hysteresis belong to the state machine,
 * which takes these verdicts as input. Same inputs, same answer, so it can be
 * tested on a Mac and replayed over old files. */
void classify(const struct feat_snapshot *f, const struct ephem_pos *sky,
              const struct site_baseline *base, bool clock_synced, struct verdict *out);

#endif
