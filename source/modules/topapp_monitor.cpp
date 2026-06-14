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

#include "topapp_monitor.h"
#include "utils/atrace.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include <spdlog/spdlog.h>

constexpr char MODULE_NAME[] = "TopappMonitor";
constexpr int64_t TOP_APP_SWITCH_DELAY_MS = 800;
constexpr size_t TOP_TASK_NR_DIFF_MIN = 10;
constexpr int64_t HEARTBEAT_INTERVAL_MS = 5000;

TopappMonitor::TopappMonitor()
    : topappNr_(0), hw_(HwCreate(MODULE_NAME)), dw_(DwCreate(MODULE_NAME)), dwHeartbeat_(DwCreate(MODULE_NAME)) {}

TopappMonitor::~TopappMonitor() {}

void TopappMonitor::Start(void) {
    using namespace std::placeholders;
    CoSubscribe("cgroup.ta.list", std::bind(&TopappMonitor::OnTopappList, this, _1));
    ScheduleHeartbeat();
}

void TopappMonitor::OnTopappList(const void *data) {
    if (CoHasSubscriber("topapp.pkgName") == false) {
        return;
    }

    const auto &pl = CoBridge::Get<PidList>(data);
    auto nr = static_cast<int>(pl.size());

    // Fast path: read /proc/{pid}/cmdline on every event (~1ms, no fork/exec)
    TryFastPathUpdate(pl);

    // Threshold check: only schedule dumpsys for large cgroup changes
    if (std::abs(nr - topappNr_) <= TOP_TASK_NR_DIFF_MIN) {
        return;
    }
    topappNr_ = nr;

    ScheduleDumpsysFallback();
}

void TopappMonitor::TryFastPathUpdate(const PidList &pl) {
    if (pl.empty()) {
        return;
    }

    // Read cmdline of the first PID in the top-app cgroup (~1ms)
    auto pkgName = GetTopAppNameProcfs(pl[0]);
    if (pkgName.empty()) {
        // Not a Java app, process died, or I/O error — let dumpsys handle it
        return;
    }

    {
        std::lock_guard<std::mutex> lk(prevPkgMut_);
        if (pkgName == prevPkgName_) {
            return;
        }
        prevPkgName_ = pkgName;
    }

    SPDLOG_DEBUG("topapp.pkgName {} (fast)", pkgName);
    CoPublish("topapp.pkgName", &pkgName);
}

void TopappMonitor::ScheduleDumpsysFallback(void) {
    auto delayed = [this]() {
        auto heavywork = [this]() { DoDumpsysUpdate(); };
        HwSetWork(hw_, heavywork);
    };
    DwSetWork(dw_, delayed, GetNowTs() + MsToUs(TOP_APP_SWITCH_DELAY_MS));
}

void TopappMonitor::DoDumpsysUpdate(void) {
    ATRACE_SCOPE(GetTopAppName);
    auto pkgName = GetTopAppNameDumpsys();
    if (pkgName.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(prevPkgMut_);
        if (pkgName == prevPkgName_) {
            return;
        }
        prevPkgName_ = pkgName;
    }

    SPDLOG_DEBUG("topapp.pkgName {}", pkgName);
    CoPublish("topapp.pkgName", &pkgName);
}

void TopappMonitor::ScheduleHeartbeat(void) {
    auto heartbeat = [this]() {
        HeartbeatCheck();
        ScheduleHeartbeat();
    };
    DwSetWork(dwHeartbeat_, heartbeat, GetNowTs() + MsToUs(HEARTBEAT_INTERVAL_MS));
}

void TopappMonitor::HeartbeatCheck(void) {
    if (CoHasSubscriber("topapp.pkgName") == false) {
        return;
    }

    auto heavywork = [this]() {
        ATRACE_SCOPE(HeartbeatDumpsys);
        auto pkgName = GetTopAppNameDumpsys();
        if (pkgName.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lk(prevPkgMut_);
            if (pkgName == prevPkgName_) {
                return;
            }
            prevPkgName_ = pkgName;
        }

        SPDLOG_DEBUG("topapp.pkgName {} (heartbeat)", pkgName);
        CoPublish("topapp.pkgName", &pkgName);
    };
    HwSetWork(hw_, heavywork);
}
