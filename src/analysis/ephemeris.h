#ifndef DSM_ANALYSIS_EPHEMERIS_H
#define DSM_ANALYSIS_EPHEMERIS_H

#include <stdint.h>

/* Where the sun and moon are, worked out locally from the time and the site.
 * No network and no stored tables: this is what lets the device say "not dark
 * yet" or "the moon is up" with the cable unplugged. Portable. Owner: Nick. */

struct ephem_site {
    double lat_deg;    /* north is positive */
    double lon_deg;    /* east is positive, so Colorado is negative */
};

/* Rounded city-center location, not the address of the device. At this
 * precision the position error is far below the accuracy targets. */
#define EPHEM_SITE_FORT_COLLINS { 40.59, -105.08 }

struct ephem_pos {
    double sun_alt_deg;    /* degrees above the horizon; negative = below */
    double moon_alt_deg;   /* same, corrected for where on Earth the site is */
    double moon_illum;     /* lit fraction of the moon, 0 (new) to 1 (full) */
};

/* Fills out for the time t_real_ns (a record's t_real_ns field: nanoseconds
 * since 1970, UTC). Altitudes are geometric, meaning without the bending of
 * light by the air.
 *
 * The answer is only as good as the clock. The caller checks that the clock
 * is synced before trusting it (see DSM_RULE_CLOCK_UNSYNC in verdict.h). */
void ephem_compute(const struct ephem_site *site, uint64_t t_real_ns, struct ephem_pos *out);

#endif
