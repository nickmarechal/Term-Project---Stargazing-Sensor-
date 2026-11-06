#include "analysis/ephemeris.h"

#include <math.h>

/* Sun and moon altitude from the low-precision formulas in The Astronomical
 * Almanac (sections C and D), good for decades around 2000. Checked against
 * NASA/JPL Horizons in tests/test_ephemeris.c.
 *
 * The plan: get each body's direction in a frame fixed to the stars, work out
 * which way is "straight up" at the site in that same frame, and measure the
 * angle between them.
 *
 * Ignored on purpose, all far below the accuracy targets: the small gap between
 * UTC and the time scales these formulas are defined in; the bending of light by
 * the air (it doesn't matter at the sun's -18 degree test); and the Earth's
 * flattening and the site's height (measured: worst moon error 0.211 degrees
 * treating the Earth as a sphere, 0.209 with both). */

#define PI 3.14159265358979323846
#define DEG (PI / 180.0)

/* Unix time of 2000-01-01 12:00 UTC, the moment these formulas count from. */
#define J2000_UNIX 946728000.0
#define DAYS_PER_CENTURY 36525.0

struct vec {
    double x, y, z;
};

static double sind(double deg) { return sin(deg * DEG); }
static double cosd(double deg) { return cos(deg * DEG); }

static double dot(struct vec a, struct vec b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* Sun and moon come as longitude and latitude along the ecliptic (the plane of
 * Earth's orbit). The Earth's rotation is about a different axis, tilted by
 * obliquity, so turn the direction into that axis's frame. Result has length 1. */
static struct vec from_ecliptic(double lon_deg, double lat_deg, double obliquity_deg)
{
    double cl = cosd(lat_deg), sl = sind(lat_deg);
    double co = cosd(lon_deg), so = sind(lon_deg);
    double ce = cosd(obliquity_deg), se = sind(obliquity_deg);
    struct vec v = {
        cl * co,
        ce * cl * so - se * sl,
        se * cl * so + ce * sl,
    };
    return v;
}

/* days: days since J2000. */
static double obliquity_deg(double days)
{
    return 23.439 - 0.0000004 * days;
}

static struct vec sun_direction(double days)
{
    double mean_lon = 280.460 + 0.9856474 * days;
    double anomaly = 357.528 + 0.9856003 * days;
    double lon = mean_lon + 1.915 * sind(anomaly) + 0.020 * sind(2.0 * anomaly);
    return from_ecliptic(lon, 0.0, obliquity_deg(days));
}

/* The moon's direction, and its parallax: the angle the Earth's radius covers
 * as seen from the moon. It is how we get the distance. */
static struct vec moon_direction(double days, double *parallax_deg)
{
    double t = days / DAYS_PER_CENTURY;
    double lon = 218.32 + 481267.881 * t
               + 6.29 * sind(135.0 + 477198.87 * t)
               - 1.27 * sind(259.3 - 413335.36 * t)
               + 0.66 * sind(235.7 + 890534.22 * t)
               + 0.21 * sind(269.9 + 954397.74 * t)
               - 0.19 * sind(357.5 + 35999.05 * t)
               - 0.11 * sind(186.5 + 966404.03 * t);
    double lat = 5.13 * sind(93.3 + 483202.02 * t)
               + 0.28 * sind(228.2 + 960400.89 * t)
               - 0.28 * sind(318.3 + 6003.15 * t)
               - 0.17 * sind(217.6 - 407332.21 * t);
    *parallax_deg = 0.9508
                  + 0.0518 * cosd(135.0 + 477198.87 * t)
                  + 0.0095 * cosd(259.3 - 413335.36 * t)
                  + 0.0078 * cosd(235.7 + 890534.22 * t)
                  + 0.0028 * cosd(269.9 + 954397.74 * t);
    return from_ecliptic(lon, lat, obliquity_deg(days));
}

/* How far the star frame has turned past the site's longitude. */
static double local_sidereal_deg(double days, double lon_deg)
{
    return 280.46061837 + 360.98564736629 * days + lon_deg;
}

/* Which way is straight up at the site, length 1. */
static struct vec zenith_at(const struct ephem_site *site, double sidereal_deg)
{
    double cl = cosd(site->lat_deg);
    struct vec v = {
        cl * cosd(sidereal_deg),
        cl * sind(sidereal_deg),
        sind(site->lat_deg),
    };
    return v;
}

/* Altitude of a direction: 90 degrees minus its angle from straight up. */
static double altitude_deg(struct vec dir, struct vec zenith)
{
    return asin(dot(dir, zenith) / sqrt(dot(dir, dir))) / DEG;
}

void ephem_compute(const struct ephem_site *site, uint64_t t_real_ns, struct ephem_pos *out)
{
    double days = ((double)t_real_ns / 1e9 - J2000_UNIX) / 86400.0;
    double sidereal = local_sidereal_deg(days, site->lon_deg);
    struct vec zenith = zenith_at(site, sidereal);

    /* The sun is so far away that where we stand on Earth makes no difference
     * we could measure, so its direction from the Earth's center will do. */
    struct vec sun = sun_direction(days);
    out->sun_alt_deg = altitude_deg(sun, zenith);

    /* The moon is close: only about 60 Earth radii. Standing on the surface
     * instead of at the center shifts it by up to about 1 degree (measured:
     * 1.034 worst case without this), so measure from the site. The site is one
     * Earth radius from the center, straight up, so its position is zenith. */
    double parallax;
    struct vec moon = moon_direction(days, &parallax);
    double distance = 1.0 / sind(parallax);
    struct vec from_site = {
        distance * moon.x - zenith.x,
        distance * moon.y - zenith.y,
        distance * moon.z - zenith.z,
    };
    out->moon_alt_deg = altitude_deg(from_site, zenith);

    /* Lit fraction from the angle between sun and moon: 180 degrees apart is
     * full, 0 is new. */
    out->moon_illum = (1.0 - dot(sun, moon)) / 2.0;
}
