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

#include "dynamic_fps.h"
#include "cobridge_type.h"
#include "utils/fmt_exception.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <memory>
#include <system_error>
#include <vector>

constexpr char MODULE_NAME[] = "DynamicFps";
constexpr int64_t DEFAULT_GESTURE_SLACK_MS = 4000;
constexpr int64_t DEFAULT_TOUCH_SLACK_MS = 4000;
constexpr int DEFAULT_ENABLE_MIN_BRIGHTNESS = 8;
constexpr bool DEFAULT_USE_SF_BACKDOOR = false;
constexpr int MIN_TOUCH_SLACK_MS = 100;
constexpr int MAX_ENABLE_MIN_BRIGHTNESS = 255;
constexpr double BRIGHTNESS_SAMPLE_INTERVAL_S = 10;
constexpr char UNIVERSIAL_PKG_NAME[] = "*";
constexpr char OFFSCREEN_PKG_NAME[] = "-";

std::string Trim(const std::string &str) {
    if (str.empty()) {
        return str;
    }
    auto firstScan = str.find_first_not_of(" \n\r");
    auto first = (firstScan == std::string::npos) ? str.length() : firstScan;
    auto last = str.find_last_not_of(" \n\r");
    return str.substr(first, last - first + 1);
}

// Strict, non-throwing integer parse: the whole token must be a valid, in-range int.
// On failure returns false and fills *reason with a short, human-readable cause.
static bool ParseStrictInt(const std::string &tok, int *out, std::string *reason) {
    if (tok.empty()) {
        *reason = "empty value";
        return false;
    }
    int value = 0;
    const char *first = tok.data();
    const char *last = tok.data() + tok.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec == std::errc::invalid_argument) {
        *reason = "not an integer";
        return false;
    }
    if (ec == std::errc::result_out_of_range) {
        *reason = "value out of range";
        return false;
    }
    if (ptr != last) {
        *reason = "trailing characters";
        return false;
    }
    *out = value;
    return true;
}

DynamicFps::DynamicFps(const std::string &configPath, const std::string &notifyPath)
    : useSfBackdoor_(DEFAULT_USE_SF_BACKDOOR),
      touchSlackMs_(DEFAULT_TOUCH_SLACK_MS),
      gestureSlackMs_(DEFAULT_GESTURE_SLACK_MS),
      enableMinBrightness_(DEFAULT_ENABLE_MIN_BRIGHTNESS),
      hasUniversial_(false),
      hasOffscreen_(false),
      sawUniversialLine_(false),
      sawOffscreenLine_(false),
      notifyPath_(notifyPath),
      touchPressed_(false),
      btnPressed_(false),
      active_(false),
      lowBrightness_(false),
      isOffscreen_(false),
      curHz_(INT32_MAX),
      forceSwitch_(false),
      dwInput_(DwCreate(MODULE_NAME)),
      dwGesture_(DwCreate(MODULE_NAME)),
      dwWakeup_(DwCreate(MODULE_NAME)),
      hw_(HwCreate(MODULE_NAME)) {
    LoadConfig(configPath);
}

void DynamicFps::Start(void) { AddReactor(); }

void DynamicFps::LoadConfig(const std::string &configPath) {
    std::unique_ptr<FILE, int (*)(FILE *)> fp(fopen(configPath.c_str(), "re"), fclose);
    if (fp == nullptr) {
        throw FmtException("Cannot open config '{}'", configPath);
    }

    // Validate the whole file first, accumulating every problem so a single bad value cannot
    // take down the service via an opaque exception, and so the user can fix all mistakes at once.
    std::vector<std::string> errors;
    char buf[256];
    int lineNo = 0;
    while (fgets(buf, sizeof(buf), fp.get()) != nullptr) {
        ++lineNo;
        auto line = Trim(buf);
        ParseLine(line, lineNo, errors);
    }

    if (sawOffscreenLine_ == false) {
        errors.emplace_back("missing offscreen rule '-' (e.g. '- -1 -1')");
    }
    if (sawUniversialLine_ == false) {
        errors.emplace_back("missing default rule '*' (e.g. '* 60 120')");
    }
    CollectInvalidRules(errors);

    if (errors.empty() == false) {
        // Pass the dynamic text as an argument (never as the format string) so a stray '{' in a
        // package name or raw line cannot break formatting.
        std::string msg = fmt::format("Invalid config '{}' ({} problem(s)):", configPath, errors.size());
        for (const auto &e : errors) {
            msg += "\n  - ";
            msg += e;
        }
        throw FmtException("{}", msg);
    }

    if (useSfBackdoor_) {
        SPDLOG_INFO("Use surfaceflinger backdoor to switch refresh rate");
    } else {
        SPDLOG_INFO("Use PEAK_REFRESH_RATE to switch refresh rate");
    }
}

void DynamicFps::ParseLine(const std::string &line, int lineNo, std::vector<std::string> &errors) {
    auto isComment = [](const std::string &line) { return line[0] == '#'; };
    auto isTunable = [](const std::string &line) { return line[0] == '/'; };

    if (line.empty() || isComment(line)) {
        return;
    }

    char name[256];
    name[0] = '\0';

    if (isTunable(line)) {
        // /touchSlackMs 4000
        char value[256];
        value[0] = '\0';
        if (sscanf(line.c_str(), "/%255s %255s", name, value) == 2) {
            SPDLOG_DEBUG("Set '{}'={}", name, value);
            SetTunable(name, value, lineNo, errors);
        } else {
            errors.push_back(fmt::format("line {}: malformed tunable '{}' (expected '/<name> <value>')", lineNo, line));
        }
    } else {
        // com.example.app 60 120
        // com.example.app 2 0
        char idleStr[256];
        char activeStr[256];
        idleStr[0] = '\0';
        activeStr[0] = '\0';
        if (sscanf(line.c_str(), "%255s %255s %255s", name, idleStr, activeStr) != 3) {
            errors.push_back(
                fmt::format("line {}: malformed rule '{}' (expected '<pkgName> <idle> <active>')", lineNo, line));
            return;
        }

        std::string pkgName = name;
        // Record that a line for this special rule was seen (even if its fields are malformed) so a
        // present-but-broken default/offscreen rule reports its field error only, not also "missing".
        if (pkgName == UNIVERSIAL_PKG_NAME) {
            sawUniversialLine_ = true;
        } else if (pkgName == OFFSCREEN_PKG_NAME) {
            sawOffscreenLine_ = true;
        }

        FpsRule rule;
        std::string reason;
        bool valid = true;
        if (ParseStrictInt(idleStr, &rule.idle, &reason) == false) {
            errors.push_back(
                fmt::format("line {}: rule '{}' has invalid idle value '{}' ({})", lineNo, pkgName, idleStr, reason));
            valid = false;
        }
        if (ParseStrictInt(activeStr, &rule.active, &reason) == false) {
            errors.push_back(fmt::format("line {}: rule '{}' has invalid active value '{}' ({})", lineNo, pkgName,
                                         activeStr, reason));
            valid = false;
        }
        if (valid) {
            AddRule(pkgName, rule);
        }
    }
}

void DynamicFps::AddRule(const std::string &pkgName, FpsRule rule) {
    auto isUniversial = [](const std::string &pkgName) { return pkgName == UNIVERSIAL_PKG_NAME; };
    auto isOffscreen = [](const std::string &pkgName) { return pkgName == OFFSCREEN_PKG_NAME; };
    if (isUniversial(pkgName)) {
        hasUniversial_ = true;
        universial_ = rule;
    } else if (isOffscreen(pkgName)) {
        hasOffscreen_ = true;
        offscreen_ = rule;
    } else {
        SPDLOG_DEBUG("Load '{}', dfps={}/{}", pkgName, rule.idle, rule.active);
        rules_.emplace(pkgName, rule);
    }
}

void DynamicFps::SetTunable(const std::string &tunable, const std::string &value, int lineNo,
                            std::vector<std::string> &errors) {
    // Parse without throwing; on a bad value record a locatable error and keep the default.
    auto parse = [&](int *out) {
        std::string reason;
        if (ParseStrictInt(value, out, &reason)) {
            return true;
        }
        errors.push_back(
            fmt::format("line {}: tunable '{}' has invalid value '{}' ({})", lineNo, tunable, value, reason));
        return false;
    };

    int parsed = 0;
    if (tunable == "useSfBackdoor") {
        if (parse(&parsed)) {
            useSfBackdoor_ = (parsed > 0) ? true : false;
        }
    } else if (tunable == "touchSlackMs") {
        if (parse(&parsed)) {
            touchSlackMs_ = std::max(MIN_TOUCH_SLACK_MS, parsed);
        }
    } else if (tunable == "enableMinBrightness") {
        if (parse(&parsed)) {
            enableMinBrightness_ = std::min(MAX_ENABLE_MIN_BRIGHTNESS, parsed);
        }
    } else {
        errors.push_back(fmt::format("line {}: unknown tunable '{}'", lineNo, tunable));
    }
}

void DynamicFps::CollectInvalidRules(std::vector<std::string> &errors) const {
    auto isDefaultRule = [](const FpsRule &rule) { return rule.idle == -1 && rule.active == -1; };
    auto isSfBackdoorRule = [](const FpsRule &rule) { return rule.idle < 20 && rule.active < 20; };
    auto isInvalid = [=](const FpsRule &rule) {
        return isDefaultRule(rule) == false && useSfBackdoor_ != isSfBackdoorRule(rule);
    };
    auto describe = [this](const std::string &name, const FpsRule &rule) {
        const char *expected = useSfBackdoor_ ? "expected Surfaceflinger backdoor indices (< 20)"
                                              : "expected PEAK_REFRESH_RATE values (>= 20)";
        return fmt::format("rule '{}' (idle={}, active={}) is incompatible with useSfBackdoor={}: {}", name, rule.idle,
                           rule.active, useSfBackdoor_ ? 1 : 0, expected);
    };

    if (hasOffscreen_ && isInvalid(offscreen_)) {
        errors.push_back(describe("offscreen", offscreen_));
    }
    if (hasUniversial_ && isInvalid(universial_)) {
        errors.push_back(describe("default", universial_));
    }
    for (const auto &[name, rule] : rules_) {
        if (isInvalid(rule)) {
            errors.push_back(describe(name, rule));
        }
    }
}

DynamicFps::FpsRule DynamicFps::GetCurrentRule(void) const {
    FpsRule rule;
    const auto &pkgName = overridedApp_.empty() ? curApp_ : overridedApp_;
    if (pkgName == OFFSCREEN_PKG_NAME) {
        rule = offscreen_;
    } else {
        auto it = rules_.find(pkgName);
        if (it != rules_.end()) {
            rule = it->second;
        } else {
            rule = universial_;
        }
    }
    return rule;
}

void DynamicFps::AddReactor(void) {
    using namespace std::placeholders;
    CoSubscribe("input.touch", std::bind(&DynamicFps::OnInputTouch, this, _1));
    CoSubscribe("input.btn", std::bind(&DynamicFps::OnInputBtn, this, _1));
    CoSubscribe("input.state", std::bind(&DynamicFps::OnInputScene, this, _1));
    CoSubscribe("topapp.pkgName", std::bind(&DynamicFps::OnTopAppSwitch, this, _1));
    CoSubscribe("offscreen.state", std::bind(&DynamicFps::OnOffscreen, this, _1));
}

void DynamicFps::OnInputTouch(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);
    touchPressed_ = pressed;
    OnInput();
}

void DynamicFps::OnInputBtn(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);
    btnPressed_ = pressed;
    OnInput();
}

void DynamicFps::OnInput(void) {
    auto pressed = touchPressed_ || btnPressed_;
    if (pressed) {
        active_ = true;
        SwitchRefreshRate();
        DwSetWork(dwInput_, nullptr, DelayedWorker::SLEEP_TS);
    } else {
        auto enterIdle = [=]() {
            active_ = false;
            SwitchRefreshRate();
        };
        DwSetWork(dwInput_, enterIdle, GetNowTs() + MsToUs(touchSlackMs_));
    }
}

void DynamicFps::OnInputScene(const void *data) {
    const auto &input = CoBridge::Get<InputData>(data);
    if (input.inGesture) {
        overridedApp_ = UNIVERSIAL_PKG_NAME;
        SwitchRefreshRate();
        DwSetWork(dwGesture_, nullptr, DelayedWorker::SLEEP_TS);
    } else {
        auto resume = [=]() {
            if (overridedApp_ == UNIVERSIAL_PKG_NAME) {
                overridedApp_ = "";
                SwitchRefreshRate();
            }
        };
        DwSetWork(dwGesture_, resume, GetNowTs() + MsToUs(gestureSlackMs_));
    }
}

void DynamicFps::OnTopAppSwitch(const void *data) {
    const auto &topApp = CoBridge::Get<std::string>(data);
    if (topApp != curApp_) {
        curApp_ = topApp;
        SwitchRefreshRate(true);
    }
}

void DynamicFps::OnOffscreen(const void *data) {
    const auto &isOff = CoBridge::Get<bool>(data);
    if (isOff == isOffscreen_) {
        return;
    }

    isOffscreen_ = isOff;
    if (isOff) {
        overridedApp_ = OFFSCREEN_PKG_NAME;
        SwitchRefreshRate(true);
    } else {
        auto exitOffscreen = [=]() {
            if (overridedApp_ == OFFSCREEN_PKG_NAME) {
                overridedApp_ = "";
                SwitchRefreshRate(true);
            }
        };
        DwSetWork(dwWakeup_, exitOffscreen, GetNowTs() + MsToUs(gestureSlackMs_));
    }
}

void DynamicFps::SwitchRefreshRate(bool force) {
    forceSwitch_ = force;
    if (active_) {
        HwSetWork(hw_, [this]() {
            auto rule = GetCurrentRule();
            SwitchRefreshRate(rule.active);
        });
    } else {
        HwSetWork(hw_, [this]() {
            auto rule = GetCurrentRule();
            if (brightnessTimer_.ElapsedS() > BRIGHTNESS_SAMPLE_INTERVAL_S) {
                brightnessTimer_.Reset();
                auto brightness = GetScreenBrightness();
                lowBrightness_ = brightness < enableMinBrightness_;
            }
            SwitchRefreshRate(lowBrightness_ ? rule.active : rule.idle);
        });
    }
}

void DynamicFps::SwitchRefreshRate(int hz) {
    auto force = forceSwitch_;
    forceSwitch_ = false;
    SPDLOG_DEBUG("switch {}", hz);
    if (force == false && hz == curHz_) {
        return;
    }

    std::string hzStr = std::to_string(hz);
    curHz_ = hz;
    NotifyRefreshRate(hzStr);
    if (useSfBackdoor_) {
        SysSurfaceflingerBackdoor(hzStr, force);
    } else {
        SysPeakRefreshRate(hzStr, force);
    }
}

void DynamicFps::NotifyRefreshRate(const std::string_view &hz) {
    int fd = open(notifyPath_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_CREAT | O_TRUNC);
    if (fd > 0) {
        WriteSysfsFile(fd, hz);
        close(fd);
    }
}
