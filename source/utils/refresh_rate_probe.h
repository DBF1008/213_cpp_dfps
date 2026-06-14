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

#pragma once

#include <string>
#include <vector>

// One entry of the display's mode list. `index` is the SurfaceFlinger config
// index, i.e. the value `service call SurfaceFlinger 1035 i32 <index>`
// (setActiveConfig) expects.
struct DisplayMode {
    int index;
    double fps;
};

// All functions in this header are pure: they take strings (command output) and
// return parsed results with no I/O, no Android headers and no logging. This is
// what lets the refresh-rate verification/fallback logic be unit tested on the
// host without a device.
//
// Every parser is intentionally tolerant. When the input does not match any
// supported pattern it returns an "unknown" sentinel (<=0 / <0 / empty / false)
// instead of guessing. The exact set of supported `dumpsys` / `settings` /
// `service` shapes is pinned by the fixtures in
// test/refresh_rate_probe_test.cpp.

// Parse the SurfaceFlinger display-config list (from `dumpsys SurfaceFlinger`)
// into (index, fps) pairs. Returns an empty vector when nothing is recognised.
std::vector<DisplayMode> ParseDisplayModes(const std::string &dumpsysSf);

// Parse the *current* refresh rate in Hz from `dumpsys SurfaceFlinger` or
// `dumpsys display`. Returns <=0 when unknown.
double ParseActiveRefreshRate(const std::string &dumpsys);

// Parse the active SurfaceFlinger config index from `dumpsys SurfaceFlinger`.
// Returns <0 when unknown.
int ParseActiveConfigIndex(const std::string &dumpsysSf);

// Parse the value printed by `cmd settings get <ns> <key>`. Trims surrounding
// whitespace and treats an empty result or the literal "null" as "no value"
// (returns false). On success copies the trimmed value into `out`.
bool ParseSettingsValue(const std::string &cmdOutput, std::string *out);

// Heuristically decide whether an Android `service call` succeeded from the
// parcel it printed. Returns false on empty output or a binder exception/error
// parcel. This is a coarse command-level guard, not a substitute for reading
// back the actual state.
bool ServiceCallSucceeded(const std::string &parcelOutput);

// True when a parsed mode table is trustworthy enough to translate index<->Hz
// safely: at least two modes, every fps within [24,240] Hz, indices contiguous
// starting from 0 (matching setActiveConfig semantics) and -- when activeHz>0 --
// some mode matching activeHz within tolerance. Pass activeHz<=0 to skip the
// last cross-check.
bool ModesAreConfident(const std::vector<DisplayMode> &modes, double activeHz);

// Index of the mode whose fps is closest to `hz`, or -1 when `modes` is empty.
int NearestModeIndexForHz(const std::vector<DisplayMode> &modes, double hz);

// fps for `index`, or <=0 when that index is not present.
double HzForIndex(const std::vector<DisplayMode> &modes, int index);

// Outcome of writing one Android setting. `required` keys must be accepted for
// the PEAK apply to count as successful; ROM-private keys (e.g. miui_*) are
// optional/best-effort so a ROM where they are meaningless never fails the
// switch.
struct SettingPutResult {
    bool required;
    bool accepted;
};

// True when every required setting was accepted (optional ones are ignored). An
// empty list returns false (nothing was applied).
bool RequiredSettingsAccepted(const std::vector<SettingPutResult> &results);
