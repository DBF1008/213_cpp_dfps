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

#include "refresh_rate_probe.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr double RATE_MIN_HZ = 20.0;
constexpr double RATE_MAX_HZ = 240.0;
constexpr double MATCH_TOLERANCE_HZ = 1.5;

// Parse a base-10 integer at `pos` (skipping leading spaces/tabs, allowing a
// sign). Returns false if no digit is found. On success writes the value and the
// one-past-end offset.
bool ParseIntAt(const std::string &s, size_t pos, int *out, size_t *endPos) {
    size_t p = pos;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) {
        ++p;
    }
    bool neg = false;
    if (p < s.size() && (s[p] == '-' || s[p] == '+')) {
        neg = (s[p] == '-');
        ++p;
    }
    if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) {
        return false;
    }
    long v = 0;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
        v = v * 10 + (s[p] - '0');
        ++p;
        if (v > 2000000000) {
            break;
        }
    }
    if (out) {
        *out = static_cast<int>(neg ? -v : v);
    }
    if (endPos) {
        *endPos = p;
    }
    return true;
}

// Parse a floating point number at `pos` using strtod (which skips leading
// whitespace). Returns false when no conversion happens.
bool ParseDoubleAt(const std::string &s, size_t pos, double *out, size_t *endPos) {
    if (pos >= s.size()) {
        return false;
    }
    const char *start = s.c_str() + pos;
    char *end = nullptr;
    double v = std::strtod(start, &end);
    if (end == start) {
        return false;
    }
    if (out) {
        *out = v;
    }
    if (endPos) {
        *endPos = static_cast<size_t>(end - s.c_str());
    }
    return true;
}

// Find the first occurrence of `key` and parse an integer right after it.
bool ExtractIntAfter(const std::string &s, const char *key, int *out) {
    size_t kp = s.find(key);
    if (kp == std::string::npos) {
        return false;
    }
    return ParseIntAt(s, kp + std::strlen(key), out, nullptr);
}

// Scan from `from` to end-of-line for the first float that looks like a refresh
// rate (within [RATE_MIN_HZ, RATE_MAX_HZ]).
double FirstRateFloatOnLine(const std::string &s, size_t from) {
    size_t eol = s.find('\n', from);
    if (eol == std::string::npos) {
        eol = s.size();
    }
    size_t p = from;
    while (p < eol) {
        if (std::isdigit(static_cast<unsigned char>(s[p]))) {
            double v = 0;
            size_t end = p;
            if (ParseDoubleAt(s, p, &v, &end)) {
                if (v >= RATE_MIN_HZ && v <= RATE_MAX_HZ) {
                    return v;
                }
                p = (end > p) ? end : p + 1;
                continue;
            }
        }
        ++p;
    }
    return -1;
}

// DisplayManager prints modes like `{id=2, width=1080, height=2400, fps=120.0}`.
// Find the record whose id matches `wantId` and return its fps.
double FpsForModeId(const std::string &s, int wantId) {
    size_t p = s.find("id=");
    while (p != std::string::npos) {
        int got = 0;
        size_t end = 0;
        if (ParseIntAt(s, p + 3, &got, &end) && got == wantId) {
            size_t limit = std::min(s.size(), end + 200);
            size_t brace = s.find('}', end);
            if (brace != std::string::npos) {
                limit = std::min(limit, brace);
            }
            size_t fp = s.find("fps=", end);
            if (fp != std::string::npos && fp < limit) {
                double fv = 0;
                if (ParseDoubleAt(s, fp + 4, &fv, nullptr) && fv > 0) {
                    return fv;
                }
            }
        }
        p = s.find("id=", p + 3);
    }
    return -1;
}

// Within a single line [b, e), recognise a leading config index in one of the
// supported shapes: "config N", "mode N" or "[N]".
bool LineConfigIndex(const std::string &s, size_t b, size_t e, int *idx) {
    static const char *kKeywords[] = {"config ", "mode ", "Config ", "Mode "};
    for (const char *kw : kKeywords) {
        size_t k = s.find(kw, b);
        if (k != std::string::npos && k < e) {
            size_t end = 0;
            if (ParseIntAt(s, k + std::strlen(kw), idx, &end) && end <= e) {
                return true;
            }
        }
    }
    size_t lb = s.find('[', b);
    if (lb != std::string::npos && lb < e) {
        size_t end = 0;
        int v = 0;
        if (ParseIntAt(s, lb + 1, &v, &end) && end < e && s[end] == ']') {
            *idx = v;
            return true;
        }
    }
    return false;
}

// Within a single line [b, e), find a float immediately followed by an "fps" or
// "Hz" unit and return it.
double LineFpsHz(const std::string &s, size_t b, size_t e) {
    size_t p = b;
    while (p < e) {
        bool startsNumber = std::isdigit(static_cast<unsigned char>(s[p])) ||
                            (s[p] == '.' && p + 1 < e && std::isdigit(static_cast<unsigned char>(s[p + 1])));
        if (startsNumber) {
            double v = 0;
            size_t end = p;
            if (ParseDoubleAt(s, p, &v, &end) && v > 0) {
                size_t q = end;
                while (q < e && s[q] == ' ') {
                    ++q;
                }
                bool isFps = s.compare(q, 3, "fps") == 0;
                bool isHz = s.compare(q, 2, "Hz") == 0 || s.compare(q, 2, "hz") == 0;
                if ((isFps || isHz) && v >= 10.0 && v <= 300.0) {
                    return v;
                }
                p = (end > p) ? end : p + 1;
                continue;
            }
        }
        ++p;
    }
    return -1;
}

} // namespace

std::vector<DisplayMode> ParseDisplayModes(const std::string &dumpsysSf) {
    std::vector<DisplayMode> modes;
    size_t pos = 0;
    while (pos <= dumpsysSf.size()) {
        size_t eol = dumpsysSf.find('\n', pos);
        size_t lineEnd = (eol == std::string::npos) ? dumpsysSf.size() : eol;
        int idx = -1;
        if (LineConfigIndex(dumpsysSf, pos, lineEnd, &idx) && idx >= 0) {
            double fps = LineFpsHz(dumpsysSf, pos, lineEnd);
            if (fps > 0) {
                modes.push_back({idx, fps});
            }
        }
        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 1;
    }
    return modes;
}

double ParseActiveRefreshRate(const std::string &dumpsys) {
    // Pattern A: a "refresh-rate"/"refreshRate" marker followed by a plausible Hz.
    static const char *kRateKeys[] = {"refresh-rate", "refreshRate", "RefreshRate"};
    for (const char *key : kRateKeys) {
        size_t p = dumpsys.find(key);
        while (p != std::string::npos) {
            double v = FirstRateFloatOnLine(dumpsys, p + std::strlen(key));
            if (v > 0) {
                return v;
            }
            p = dumpsys.find(key, p + 1);
        }
    }
    // Pattern B: DisplayManager active mode -> fps.
    int activeId = -1;
    if (ExtractIntAfter(dumpsys, "mActiveModeId=", &activeId) && activeId >= 0) {
        double v = FpsForModeId(dumpsys, activeId);
        if (v > 0) {
            return v;
        }
    }
    return -1;
}

int ParseActiveConfigIndex(const std::string &dumpsysSf) {
    int v = 0;
    // Match the common shapes: "activeConfig=N", the capitalised "ActiveConfig=N"
    // (e.g. inside "mActiveConfig=N"), and "ActiveConfig: N".
    static const char *kKeys[] = {"activeConfig=", "ActiveConfig=", "ActiveConfig: ", "ActiveConfig:"};
    for (const char *key : kKeys) {
        if (ExtractIntAfter(dumpsysSf, key, &v) && v >= 0) {
            return v;
        }
    }
    return -1;
}

bool ParseSettingsValue(const std::string &cmdOutput, std::string *out) {
    size_t b = 0;
    size_t e = cmdOutput.size();
    while (b < e && std::isspace(static_cast<unsigned char>(cmdOutput[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(cmdOutput[e - 1]))) {
        --e;
    }
    std::string v = cmdOutput.substr(b, e - b);
    if (v.empty() || v == "null") {
        return false;
    }
    if (out) {
        *out = v;
    }
    return true;
}

bool ServiceCallSucceeded(const std::string &parcelOutput) {
    if (parcelOutput.find("Parcel(") == std::string::npos) {
        return false;
    }
    if (parcelOutput.find("Exception") != std::string::npos) {
        return false;
    }
    // Binder error parcels typically start with the 0xffffffff status word.
    if (parcelOutput.find("Parcel(ffffffff") != std::string::npos) {
        return false;
    }
    return true;
}

bool ModesAreConfident(const std::vector<DisplayMode> &modes, double activeHz) {
    if (modes.size() < 2) {
        return false;
    }
    std::vector<DisplayMode> sorted = modes;
    std::sort(sorted.begin(), sorted.end(),
              [](const DisplayMode &a, const DisplayMode &b) { return a.index < b.index; });
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i].index != static_cast<int>(i)) {
            return false; // not contiguous from 0
        }
        if (sorted[i].fps < 24.0 || sorted[i].fps > 240.0) {
            return false;
        }
    }
    if (activeHz > 0) {
        bool matched = false;
        for (const auto &dm : sorted) {
            if (std::fabs(dm.fps - activeHz) <= MATCH_TOLERANCE_HZ) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

int NearestModeIndexForHz(const std::vector<DisplayMode> &modes, double hz) {
    int best = -1;
    double bestDelta = 1e18;
    for (const auto &dm : modes) {
        double d = std::fabs(dm.fps - hz);
        if (d < bestDelta) {
            bestDelta = d;
            best = dm.index;
        }
    }
    return best;
}

double HzForIndex(const std::vector<DisplayMode> &modes, int index) {
    for (const auto &dm : modes) {
        if (dm.index == index) {
            return dm.fps;
        }
    }
    return -1;
}

bool RequiredSettingsAccepted(const std::vector<SettingPutResult> &results) {
    if (results.empty()) {
        return false;
    }
    for (const auto &r : results) {
        if (r.required && !r.accepted) {
            return false;
        }
    }
    return true;
}
