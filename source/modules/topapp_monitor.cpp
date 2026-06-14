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
#include "topapp_switch_detector.h"
#include "utils/atrace.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include <spdlog/spdlog.h>

constexpr char MODULE_NAME[] = "TopappMonitor";
constexpr int64_t TOP_APP_SWITCH_DELAY_MS = 800;

TopappMonitor::TopappMonitor() : hw_(HwCreate(MODULE_NAME)), dw_(DwCreate(MODULE_NAME)) {}

TopappMonitor::~TopappMonitor() {}

void TopappMonitor::Start(void) {
    using namespace std::placeholders;
    CoSubscribe("cgroup.ta.list", std::bind(&TopappMonitor::OnTopappList, this, _1));
}

void TopappMonitor::OnTopappList(const void *data) {
    if (CoHasSubscriber("topapp.pkgName") == false) {
        return;
    }

    const auto &pl = CoBridge::Get<PidList>(data);

    // Re-query the foreground package whenever the top-app task set looks like it
    // now belongs to a different app. TopappTasksChanged() combines a thread-count
    // delta with task-set churn, so an app<->app switch (as well as a launcher
    // switch or a gesture-back to another app) is caught even when the two
    // foregrounds have a similar number of threads -- the case the old count-only
    // check let slip through, leaving per-app rules stuck on the stale package.
    if (TopappTasksChanged(prevPids_, pl) == false) {
        return;
    }
    // Anchor future churn comparisons to this accepted snapshot. Comparing
    // against the last accepted set (rather than the immediately previous one)
    // also accumulates slow, piecemeal task replacement into an eventual switch.
    prevPids_ = pl;

    // A switch emits a burst of cgroup updates in quick succession; DwSetWork
    // keeps only the most recent, so the heavy dumpsys runs ~once per quiet
    // window, and the prevPkgName_ guard suppresses redundant publishes.
    auto delayed = [this]() {
        auto heavywork = [this]() {
            ATRACE_SCOPE(GetTopAppName);
            auto pkgName = GetTopAppNameDumpsys();
            if (pkgName.empty()) {
                return;
            }
            if (pkgName != prevPkgName_) {
                prevPkgName_ = pkgName;
                SPDLOG_DEBUG("topapp.pkgName {}", pkgName);
                CoPublish("topapp.pkgName", &pkgName);
            }
        };
        HwSetWork(hw_, heavywork);
    };
    DwSetWork(dw_, delayed, GetNowTs() + MsToUs(TOP_APP_SWITCH_DELAY_MS));
}
