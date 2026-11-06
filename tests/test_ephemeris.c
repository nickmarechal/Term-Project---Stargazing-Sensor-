#include "check.h"
#include "suites.h"
#include "analysis/ephemeris.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

/* Our math against NASA/JPL Horizons, at dates spread over 2026-2028.
 * Targets are DESIGN.md evaluation #12: 0.1 degrees sun, 0.5 degrees moon.
 * DESIGN.md sets no target for the lit fraction, so 0.02 is our own limit. */
#define SUN_LIMIT_DEG   0.1
#define MOON_LIMIT_DEG  0.5
#define ILLUM_LIMIT     0.02

struct ephem_ref {
    int64_t unix_s;
    double sun_alt_deg;
    double moon_alt_deg;
    double moon_illum;
};

#include "data/ephemeris_reference.inc"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct worst {
    double sun, moon, illum;
};

static void check_table(const char *name, const struct ephem_site *site,
                        const struct ephem_ref *ref, size_t n, struct worst *w)
{
    for (size_t i = 0; i < n; i++) {
        struct ephem_pos pos;
        ephem_compute(site, (uint64_t)ref[i].unix_s * 1000000000u, &pos);

        double sun = fabs(pos.sun_alt_deg - ref[i].sun_alt_deg);
        double moon = fabs(pos.moon_alt_deg - ref[i].moon_alt_deg);
        double illum = fabs(pos.moon_illum - ref[i].moon_illum);

        CHECK(sun <= SUN_LIMIT_DEG, "%s[%zu] t=%lld sun %.3f, JPL %.3f", name, i,
              (long long)ref[i].unix_s, pos.sun_alt_deg, ref[i].sun_alt_deg);
        CHECK(moon <= MOON_LIMIT_DEG, "%s[%zu] t=%lld moon %.3f, JPL %.3f", name, i,
              (long long)ref[i].unix_s, pos.moon_alt_deg, ref[i].moon_alt_deg);
        CHECK(illum <= ILLUM_LIMIT, "%s[%zu] t=%lld lit %.3f, JPL %.3f", name, i,
              (long long)ref[i].unix_s, pos.moon_illum, ref[i].moon_illum);

        if (sun > w->sun) w->sun = sun;
        if (moon > w->moon) w->moon = moon;
        if (illum > w->illum) w->illum = illum;
    }
}

/* The reference table must keep covering the cases that matter, or a pass here
 * would mean less than it appears to. */
static void test_table_covers_the_cases(void)
{
    size_t n = COUNT(REF_FORT_COLLINS), sun_dark = 0, moon_low = 0, moon_up = 0;
    CHECK(n >= 50, "only %zu dates; DESIGN.md #12 asks for at least 50", n);
    for (size_t i = 0; i < n; i++) {
        if (REF_FORT_COLLINS[i].sun_alt_deg < -18.0) sun_dark++;
        if (fabs(REF_FORT_COLLINS[i].moon_alt_deg) < 15.0) moon_low++;
        if (REF_FORT_COLLINS[i].moon_alt_deg > 0.0) moon_up++;
    }
    CHECK(sun_dark >= 10, "only %zu dates after dark", sun_dark);
    CHECK(moon_low >= 5, "only %zu dates with the moon near the horizon, where "
          "the correction for the site's position is largest", moon_low);
    CHECK(moon_up >= 10 && moon_up <= n - 10, "%zu of %zu dates with the moon up", moon_up, n);
}

void test_ephemeris(void)
{
    const struct ephem_site fort_collins = EPHEM_SITE_FORT_COLLINS;
    const struct ephem_site sydney = { -33.87, 151.21 };
    struct worst w = { 0.0, 0.0, 0.0 };

    test_table_covers_the_cases();
    check_table("fort_collins", &fort_collins, REF_FORT_COLLINS, COUNT(REF_FORT_COLLINS), &w);
    check_table("sydney", &sydney, REF_SYDNEY, COUNT(REF_SYDNEY), &w);

    printf("ephemeris vs JPL Horizons, %zu dates: worst sun %.3f deg, moon %.3f deg, lit %.4f\n",
           COUNT(REF_FORT_COLLINS) + COUNT(REF_SYDNEY), w.sun, w.moon, w.illum);
}
