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

constexpr char MODULE_NAME[] = "DynamicFps";
constexpr int64_t DEFAULT_GESTURE_SLACK_MS = 4000;
constexpr double BRIGHTNESS_SAMPLE_INTERVAL_S = 10;
constexpr char UNIVERSIAL_PKG_NAME[] = "*";
constexpr char OFFSCREEN_PKG_NAME[] = "-";

DynamicFps::DynamicFps(const std::string &configPath, const std::string &notifyPath)
    : useSfBackdoor_(false),
      touchSlackMs_(4000),
      gestureSlackMs_(DEFAULT_GESTURE_SLACK_MS),
      enableMinBrightness_(8),
      hasUniversial_(false),
      hasOffscreen_(false),
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
    auto cfg = ParseConfigFile(configPath);

    useSfBackdoor_ = cfg.useSfBackdoor;
    touchSlackMs_ = cfg.touchSlackMs;
    enableMinBrightness_ = cfg.enableMinBrightness;
    rules_ = std::move(cfg.rules);
    offscreen_ = cfg.offscreen;
    universial_ = cfg.universial;
    hasUniversial_ = cfg.hasUniversial;
    hasOffscreen_ = cfg.hasOffscreen;

    if (useSfBackdoor_) {
        SPDLOG_INFO("Use surfaceflinger backdoor to switch refresh rate");
    } else {
        SPDLOG_INFO("Use PEAK_REFRESH_RATE to switch refresh rate");
    }
}

FpsRule DynamicFps::GetCurrentRule(void) const {
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
