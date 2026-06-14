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

// constexpr char MODULE_NAME[] = "OffscreenMonitor";
constexpr int64_t KERNEL_POLL_INTERVAL_MS = 2000;

OffscreenMonitor::OffscreenMonitor(void)
    : prevOffscreen_(false),
      dwKernelPoll_(DwCreate("OffscreenMonitor")),
      signals_{} {
    // Initialize signals to safe defaults
    signals_.kernelDisplay = DisplayState::UNKNOWN;
    signals_.touchActive = false;
    signals_.buttonActive = false;
    signals_.restrictedCgroupCount = 0;
    signals_.nowMs = 0;
}

OffscreenMonitor::~OffscreenMonitor(void) {}

void OffscreenMonitor::Start(void) {
    using namespace std::placeholders;

    // Subscribe to restricted cgroup PID list (existing signal)
    CoSubscribe("cgroup.re.list", std::bind(&OffscreenMonitor::OnRestrictedList, this, _1));

    // Subscribe to input signals for hard-override detection
    CoSubscribe("input.touch", std::bind(&OffscreenMonitor::OnTouchState, this, _1));
    CoSubscribe("input.btn", std::bind(&OffscreenMonitor::OnButtonState, this, _1));

    // Start periodic kernel state polling if a sysfs source is available
    if (kernelReader_.IsAvailable()) {
        ScheduleKernelPoll();
    } else {
        SPDLOG_WARN("OffscreenMonitor: kernel display state unavailable, "
                     "relying on cgroup + input only");
    }
}

void OffscreenMonitor::OnRestrictedList(const void *data) {
    auto pids = CoBridge::Get<PidList>(data);

    std::lock_guard<std::mutex> lock(mutex_);
    signals_.restrictedCgroupCount = static_cast<int>(pids.size());
    signals_.nowMs = UsToMs(GetNowTs());
    EvaluateAndPublish();
}

void OffscreenMonitor::OnTouchState(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);

    std::lock_guard<std::mutex> lock(mutex_);
    signals_.touchActive = pressed;
    signals_.nowMs = UsToMs(GetNowTs());
    EvaluateAndPublish();
}

void OffscreenMonitor::OnButtonState(const void *data) {
    const auto &pressed = CoBridge::Get<bool>(data);

    std::lock_guard<std::mutex> lock(mutex_);
    signals_.buttonActive = pressed;
    signals_.nowMs = UsToMs(GetNowTs());
    EvaluateAndPublish();
}

void OffscreenMonitor::OnKernelPoll(void) {
    DisplayState ds = kernelReader_.Read();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        signals_.kernelDisplay = ds;
        signals_.nowMs = UsToMs(GetNowTs());
        EvaluateAndPublish();
    }

    // Reschedule next poll
    ScheduleKernelPoll();
}

void OffscreenMonitor::ScheduleKernelPoll(void) {
    int64_t fireTs = GetNowTs() + MsToUs(KERNEL_POLL_INTERVAL_MS);
    DwSetWork(dwKernelPoll_, [this]() { OnKernelPoll(); }, fireTs);
}

void OffscreenMonitor::EvaluateAndPublish(void) {
    // MUST be called with mutex_ held

    auto newState = engine_.Update(signals_);
    bool isOffscreen = (newState == OffscreenState::OFF);

    if (isOffscreen != prevOffscreen_) {
        prevOffscreen_ = isOffscreen;

        SPDLOG_DEBUG("offscreen.state {} (baseline={}, calibrated={})",
                     isOffscreen, engine_.GetCalibratedBaseline(), engine_.IsCalibrated());

        // Release mutex before publishing to avoid potential re-entrancy issues
        // with DynamicFps callbacks running synchronously on this thread.
        mutex_.unlock();
        CoPublish("offscreen.state", &isOffscreen);
        mutex_.lock();
    }
}
