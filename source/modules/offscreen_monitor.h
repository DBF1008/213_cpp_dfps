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

#include "cobridge_type.h"
#include "offscreen_policy.h"
#include "platform/module_base.h"
#include <mutex>

// OffscreenMonitor is the I/O adapter around the pure OffscreenPolicy. It feeds the policy three
// signals -- the `restricted` cgroup task count (cgroup.re.list), user input edges (input.touch /
// input.btn) and the authoritative system screen state (dumpsys, via GetScreenState()) -- and
// publishes the fused verdict on "offscreen.state" (a bool, the contract DynamicFps consumes).
//
// All decision state lives in policy_, guarded by mut_. Each handler decides under the lock, then
// performs the returned side effects (publish / authoritative query / tick rearm) AFTER releasing
// it -- CoPublish runs subscribers synchronously, so it must not be called while holding mut_.
class OffscreenMonitor : public ModuleBase {
public:
    OffscreenMonitor();
    ~OffscreenMonitor();
    void Start(void) override;

private:
    void OnRestrictedList(const void *data);
    void OnInput(const void *data);
    void ScheduleTick(int64_t delayUs);
    void RunAuthoritativeQuery(void);
    void Apply(const OffscreenAction &action);

    OffscreenPolicy policy_;
    std::mutex mut_;
    HeavyWorker::Handle hw_;
    DelayedWorker::Handle dwTick_;
};
