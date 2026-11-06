#include "check.h"
#include "suites.h"
#include "analysis/ephemeris.h"
#include "analysis/features.h"
#include "analysis/verdict.h"
#include "common/record.h"

/* These numbers are stored on disk in FEATURE and STATE records, and both
 * partners' code depends on them. Changing one would silently change what
 * old files mean, so each is pinned here. New values go on the end. */

static void test_state_numbers(void)
{
    CHECK(DSM_STATE_BOOT == 1, "BOOT %d", DSM_STATE_BOOT);
    CHECK(DSM_STATE_DAYLIGHT == 2, "DAYLIGHT %d", DSM_STATE_DAYLIGHT);
    CHECK(DSM_STATE_TWILIGHT == 3, "TWILIGHT %d", DSM_STATE_TWILIGHT);
    CHECK(DSM_STATE_UNKNOWN == 4, "UNKNOWN %d", DSM_STATE_UNKNOWN);
    CHECK(DSM_STATE_POOR == 5, "POOR %d", DSM_STATE_POOR);
    CHECK(DSM_STATE_MARGINAL == 6, "MARGINAL %d", DSM_STATE_MARGINAL);
    CHECK(DSM_STATE_GOOD == 7, "GOOD %d", DSM_STATE_GOOD);
}

static void test_rule_bits(void)
{
    CHECK(DSM_RULE_SUN_UP == 1u << 0, "SUN_UP 0x%x", DSM_RULE_SUN_UP);
    CHECK(DSM_RULE_TWILIGHT == 1u << 1, "TWILIGHT 0x%x", DSM_RULE_TWILIGHT);
    CHECK(DSM_RULE_SENSOR_MISSING == 1u << 2, "SENSOR_MISSING 0x%x", DSM_RULE_SENSOR_MISSING);
    CHECK(DSM_RULE_LOW_DATA == 1u << 3, "LOW_DATA 0x%x", DSM_RULE_LOW_DATA);
    CHECK(DSM_RULE_BASELINE_IMMATURE == 1u << 4, "BASELINE_IMMATURE 0x%x",
          DSM_RULE_BASELINE_IMMATURE);
    CHECK(DSM_RULE_CLOCK_UNSYNC == 1u << 5, "CLOCK_UNSYNC 0x%x", DSM_RULE_CLOCK_UNSYNC);
    CHECK(DSM_RULE_OVERCAST == 1u << 6, "OVERCAST 0x%x", DSM_RULE_OVERCAST);
    CHECK(DSM_RULE_DEW == 1u << 7, "DEW 0x%x", DSM_RULE_DEW);
    CHECK(DSM_RULE_BROKEN_CLOUD == 1u << 8, "BROKEN_CLOUD 0x%x", DSM_RULE_BROKEN_CLOUD);
    CHECK(DSM_RULE_UNSTABLE_SKY == 1u << 9, "UNSTABLE_SKY 0x%x", DSM_RULE_UNSTABLE_SKY);
    CHECK(DSM_RULE_MOONLIGHT == 1u << 10, "MOONLIGHT 0x%x", DSM_RULE_MOONLIGHT);
    CHECK(DSM_RULE_NOT_DARK_ENOUGH == 1u << 11, "NOT_DARK_ENOUGH 0x%x", DSM_RULE_NOT_DARK_ENOUGH);
}

static void test_feature_ids(void)
{
    CHECK(DSM_FEAT_SKY_REL == 1, "SKY_REL %d", DSM_FEAT_SKY_REL);
    CHECK(DSM_FEAT_SKY_SIGMA == 2, "SKY_SIGMA %d", DSM_FEAT_SKY_SIGMA);
    CHECK(DSM_FEAT_DT == 3, "DT %d", DSM_FEAT_DT);
    CHECK(DSM_FEAT_DT_CORR == 4, "DT_CORR %d", DSM_FEAT_DT_CORR);
    CHECK(DSM_FEAT_DT_SIGMA == 5, "DT_SIGMA %d", DSM_FEAT_DT_SIGMA);
    CHECK(DSM_FEAT_DEW_MARGIN == 6, "DEW_MARGIN %d", DSM_FEAT_DEW_MARGIN);
    CHECK(DSM_FEAT_PRESS_TREND == 7, "PRESS_TREND %d", DSM_FEAT_PRESS_TREND);
    CHECK(DSM_FEAT_SUN_ALT == 8, "SUN_ALT %d", DSM_FEAT_SUN_ALT);
    CHECK(DSM_FEAT_MOON_ALT == 9, "MOON_ALT %d", DSM_FEAT_MOON_ALT);
    CHECK(DSM_FEAT_MOON_ILLUM == 10, "MOON_ILLUM %d", DSM_FEAT_MOON_ILLUM);
}

static void test_clock_flag(void)
{
    /* Bit 4 was promised in docs/record-and-ipc.md, and it must not collide
     * with the four flags already written to files. */
    CHECK(DSM_FLAG_CLOCK_UNSYNC == 1u << 4, "CLOCK_UNSYNC 0x%x", DSM_FLAG_CLOCK_UNSYNC);
}

static void test_site(void)
{
    /* Rounded city-center Fort Collins, on purpose: not anyone's home. */
    const struct ephem_site site = EPHEM_SITE_FORT_COLLINS;
    CHECK(site.lat_deg > 40.0 && site.lat_deg < 41.0, "lat %f", site.lat_deg);
    CHECK(site.lon_deg > -106.0 && site.lon_deg < -105.0, "lon %f (west is negative)", site.lon_deg);
}

static void test_sensor_table(void)
{
    CHECK(FEAT_SENSOR_COUNT == 3, "sensors %d", FEAT_SENSOR_COUNT);
}

void test_verdict(void)
{
    test_state_numbers();
    test_rule_bits();
    test_feature_ids();
    test_clock_flag();
    test_site();
    test_sensor_table();
}
