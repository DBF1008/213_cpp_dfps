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

#include "offscreen_monitor.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include <spdlog/spdlog.h>

constexpr char MODULE_NAME[] = "OffscreenMonitor";
// First safety-net poll, fired async after every module's Start() (so DynamicFps has already
// subscribed). On a healthy device a cgroup.re.list event usually arrives sooner and kicks off
// calibration; this is the backstop for devices that never produce that event.
constexpr int64_t INITIAL_TICK_DELAY_MS = 3000;

OffscreenMonitor::OffscreenMonitor() : hw_(HwCreate(MODULE_NAME)), dwTick_(DwCreate(MODULE_NAME)) {}

OffscreenMonitor::~OffscreenMonitor() {}

void OffscreenMonitor::Start(void) {
    using namespace std::placeholders;
    // Keep subscribing to cgroup.re.list: it is what keeps CgroupListener's restricted updater
    // publishing (the updater early-returns when the topic has no subscriber).
    CoSubscribe("cgroup.re.list", std::bind(&OffscreenMonitor::OnRestrictedList, this, _1));
    CoSubscribe("input.touch", std::bind(&OffscreenMonitor::OnInput, this, _1));
    CoSubscribe("input.btn", std::bind(&OffscreenMonitor::OnInput, this, _1));
    ScheduleTick(MsToUs(INITIAL_TICK_DELAY_MS));
}

void OffscreenMonitor::OnRestrictedList(const void *data) {
    auto count = static_cast<int>(CoBridge::Get<PidList>(data).size());
    OffscreenAction action;
    {
        std::lock_guard<std::mutex> lk(mut_);
        action = policy_.OnRestrictedCount(count, GetNowTs());
    }
    Apply(action);
}

void OffscreenMonitor::OnInput(const void *) {
    OffscreenAction action;
    {
        std::lock_guard<std::mutex> lk(mut_);
        action = policy_.OnInput(GetNowTs());
    }
    Apply(action);
}

void OffscreenMonitor::ScheduleTick(int64_t delayUs) {
    auto tick = [this]() {
        OffscreenAction action;
        {
            std::lock_guard<std::mutex> lk(mut_);
            action = policy_.OnTick(GetNowTs());
        }
        Apply(action);
    };
    DwSetWork(dwTick_, tick, GetNowTs() + delayUs);
}

void OffscreenMonitor::RunAuthoritativeQuery(void) {
    auto query = [this]() {
        // GetScreenState() shells out to dumpsys (~tens of ms), so it runs on the HeavyWorker
        // thread, off the event hot path.
        auto state = GetScreenState();
        OffscreenAction action;
        {
            std::lock_guard<std::mutex> lk(mut_);
            action = policy_.OnAuthoritative(state, GetNowTs());
        }
        Apply(action);
    };
    HwSetWork(hw_, query);
}

void OffscreenMonitor::Apply(const OffscreenAction &action) {
    // Must run WITHOUT holding mut_. queryAuthoritative/rearmTick only schedule work on other
    // threads (no re-entrancy), and CoPublish invokes subscribers synchronously on this thread.
    if (action.queryAuthoritative) {
        RunAuthoritativeQuery();
    }
    if (action.rearmTickUs > 0) {
        ScheduleTick(action.rearmTickUs);
    }
    if (action.publish) {
        bool value = action.offscreenValue; // stack-local; the subscriber copies it synchronously
        SPDLOG_DEBUG("offscreen.state {}", value);
        CoPublish("offscreen.state", &value);
    }
}
