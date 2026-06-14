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
#include "kernel_state_reader.h"
#include "offscreen_state_fusion.h"
#include "platform/module_base.h"
#include <mutex>

class OffscreenMonitor : public ModuleBase {
public:
    OffscreenMonitor(void);
    ~OffscreenMonitor(void);
    void Start(void) override;

private:
    // CoBridge subscription handlers (called from different threads)
    void OnRestrictedList(const void *data);
    void OnTouchState(const void *data);
    void OnButtonState(const void *data);

    // Kernel display state polling
    void OnKernelPoll(void);
    void ScheduleKernelPoll(void);

    // Evaluate fusion engine and publish if state changed
    void EvaluateAndPublish(void);

    OffscreenStateFusion engine_;
    KernelStateReader kernelReader_;

    // Cached signal snapshot — updated by each callback under lock
    FusionSignals signals_;

    // Previous published state for edge detection
    bool prevOffscreen_;

    // Serializes access to engine_ and signals_ across CoBridge callback threads
    std::mutex mutex_;

    // DelayedWorker handle for periodic kernel state polling
    DelayedWorker::Handle dwKernelPoll_;
};
