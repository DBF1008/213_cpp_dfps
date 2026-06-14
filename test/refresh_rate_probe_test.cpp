/*
 * Copyright (C) 2021-2022 Matt Yang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "test_framework.h"
#include "utils/refresh_rate_probe.h"

// --- fixtures: representative command output shapes the parsers must handle ---

// `dumpsys SurfaceFlinger`, bracketed config list + refresh-rate line.
static const char *SF_DUMP_BRACKET = R"(
SurfaceFlinger global state:
+ DisplayDevice: "Built-in Screen"
   activeConfig=2
   refresh-rate         : 120.000008
   Display configs:
      [0] 1080 x 2400, 60.000004 fps, group 0
      [1] 1080 x 2400, 90.000003 fps, group 0
      [2] 1080 x 2400, 120.000008 fps, group 0
)";

// `dumpsys SurfaceFlinger`, "config N:" list + "Hz" unit, no refresh-rate line.
static const char *SF_DUMP_CONFIG = R"(
Display 0 state:
   ActiveConfig: 1
   modes:
      config 0: 1080 x 2400, 60.0 Hz
      config 1: 1080 x 2400, 120.0 Hz
)";

// `dumpsys display`, DisplayManager mode records.
static const char *DISP_DUMP = R"(
DISPLAY MANAGER (dumpsys display)
  mActiveModeId=3
  mSupportedModes=[{id=1, width=1080, height=2400, fps=60.000004}, {id=2, width=1080, height=2400, fps=90.0}, {id=3, width=1080, height=2400, fps=120.00001}]
)";

TEST(probe_active_config_index) {
    CHECK_EQ(ParseActiveConfigIndex(SF_DUMP_BRACKET), 2);
    CHECK_EQ(ParseActiveConfigIndex(SF_DUMP_CONFIG), 1);
    CHECK_EQ(ParseActiveConfigIndex("nothing here"), -1);
    CHECK_EQ(ParseActiveConfigIndex("mActiveConfig=4 trailing"), 4); // tolerates the m-prefix
}

TEST(probe_active_refresh_rate) {
    CHECK_NEAR(ParseActiveRefreshRate(SF_DUMP_BRACKET), 120.0, 0.5); // from refresh-rate line
    CHECK_NEAR(ParseActiveRefreshRate(DISP_DUMP), 120.0, 0.5);       // from active mode id=3
    // No rate marker at all -> unknown, so verification degrades gracefully.
    CHECK(ParseActiveRefreshRate(SF_DUMP_CONFIG) <= 0);
    CHECK(ParseActiveRefreshRate("garbage with no numbers") <= 0);
}

TEST(probe_display_modes_bracket) {
    auto modes = ParseDisplayModes(SF_DUMP_BRACKET);
    CHECK_EQ(modes.size(), static_cast<size_t>(3));
    if (modes.size() == 3) {
        CHECK_EQ(modes[0].index, 0);
        CHECK_NEAR(modes[0].fps, 60.0, 0.5);
        CHECK_EQ(modes[1].index, 1);
        CHECK_NEAR(modes[1].fps, 90.0, 0.5);
        CHECK_EQ(modes[2].index, 2);
        CHECK_NEAR(modes[2].fps, 120.0, 0.5);
    }
    // The "activeConfig=2" and "refresh-rate" lines must not leak into the table.
    CHECK(ModesAreConfident(modes, 120.0));
}

TEST(probe_display_modes_config_keyword) {
    auto modes = ParseDisplayModes(SF_DUMP_CONFIG);
    CHECK_EQ(modes.size(), static_cast<size_t>(2));
    if (modes.size() == 2) {
        CHECK_EQ(modes[0].index, 0);
        CHECK_NEAR(modes[0].fps, 60.0, 0.5);
        CHECK_EQ(modes[1].index, 1);
        CHECK_NEAR(modes[1].fps, 120.0, 0.5);
    }
    CHECK(ModesAreConfident(modes, -1)); // no activeHz cross-check available
}

TEST(probe_modes_confidence_rejects_bad_tables) {
    CHECK(!ModesAreConfident({}, 120.0));                                  // empty
    CHECK(!ModesAreConfident({{0, 60.0}}, 60.0));                          // single mode
    CHECK(!ModesAreConfident({{0, 60.0}, {2, 120.0}}, 120.0));            // gap in indices
    CHECK(!ModesAreConfident({{0, 5.0}, {1, 120.0}}, 120.0));             // implausible fps
    CHECK(!ModesAreConfident({{0, 60.0}, {1, 120.0}}, 45.0));             // active rate not in table
    CHECK(ModesAreConfident({{1, 120.0}, {0, 60.0}}, 60.0));             // unsorted but valid
}

TEST(probe_translation_helpers) {
    std::vector<DisplayMode> modes = {{0, 60.0}, {1, 90.0}, {2, 120.0}};
    CHECK_EQ(NearestModeIndexForHz(modes, 119.0), 2);
    CHECK_EQ(NearestModeIndexForHz(modes, 61.0), 0);
    CHECK_EQ(NearestModeIndexForHz(modes, 88.0), 1);
    CHECK_EQ(NearestModeIndexForHz({}, 60.0), -1);
    CHECK_NEAR(HzForIndex(modes, 2), 120.0, 0.5);
    CHECK(HzForIndex(modes, 9) <= 0);
}

TEST(probe_settings_value) {
    std::string v;
    CHECK(ParseSettingsValue("120\n", &v));
    CHECK_EQ(v, std::string("120"));
    CHECK(ParseSettingsValue("  90  \n", &v));
    CHECK_EQ(v, std::string("90"));
    CHECK(!ParseSettingsValue("null\n", &v)); // unset setting
    CHECK(!ParseSettingsValue("   \n", &v));   // blank
    CHECK(!ParseSettingsValue("", &v));        // empty
}

TEST(probe_service_call_success) {
    CHECK(ServiceCallSucceeded("Result: Parcel(00000000  '....')\n"));
    CHECK(!ServiceCallSucceeded(""));                                       // no output
    CHECK(!ServiceCallSucceeded("Result: Parcel(ffffffff ffffffe1 ...)\n")); // error status word
    CHECK(!ServiceCallSucceeded("Result: Parcel(... ServiceSpecificException ...)\n"));
}
