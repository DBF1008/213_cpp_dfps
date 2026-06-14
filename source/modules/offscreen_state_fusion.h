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

#include "cgroup_calibrator.h"
#include <cstdint>

// Kernel display power state, read from sysfs or system properties.
enum class DisplayState : uint8_t {
    ON,      // Display is powered / unblanked
    OFF,     // Display is blanked / powered down
    UNKNOWN  // Sysfs unreadable or value unrecognised
};

// Snapshot of all signals consumed by the fusion engine.
// Passed by value on every Update() call — no shared state, fully deterministic.
struct FusionSignals {
    int restrictedCgroupCount;  // PID count from cgroup.re.list
    DisplayState kernelDisplay; // From KernelStateReader
    bool touchActive;           // From input.touch
    bool buttonActive;          // From input.btn
    int64_t nowMs;              // Monotonic timestamp in milliseconds
};

// High-level offscreen state output by the engine.
enum class OffscreenState : uint8_t {
    ON,          // Screen is on (publish offscreen.state = false)
    PENDING_OFF, // Signals suggest screen-off, waiting for dwell confirmation
    OFF          // Screen confirmed off (publish offscreen.state = true)
};

struct EngineConfig {
    int64_t pendingDwellMs = 3000;  // Time to stay in PENDING_OFF before confirming OFF
    int64_t inputQuietMs = 2000;    // Required silence period after last input release
    CgroupCalibrator::Config calibConfig;
};

// Pure decision engine: no I/O, no threads, no globals.
// Feed it FusionSignals, get back OffscreenState.
//
// State machine:
//   ON ──(cgroup>hi, kernel≠ON, input quiet ≥inputQuietMs)──▶ PENDING_OFF
//   PENDING_OFF ──(dwell expires, no override)──▶ OFF
//   * ──(touch, btn, or kernel=ON)──▶ ON   (hard override, immediate)
//   OFF/PENDING_OFF ──(cgroup < loThreshold)──▶ ON
class OffscreenStateFusion {
public:
    OffscreenStateFusion(void);
    explicit OffscreenStateFusion(EngineConfig cfg);

    // Evaluate signals and return the resulting state.
    // Call this on every signal change AND periodically for dwell checks.
    OffscreenState Update(const FusionSignals &signals);

    OffscreenState GetState(void) const;
    int GetCalibratedBaseline(void) const;
    bool IsCalibrated(void) const;
    int64_t GetPendingSinceMs(void) const;

private:
    bool HasHardOverrideToOn(const FusionSignals &s) const;
    bool CgroupExceedsHi(const FusionSignals &s) const;
    bool CgroupBelowLo(const FusionSignals &s) const;
    bool IsInputQuiet(const FusionSignals &s) const;
    int64_t GetEffectiveDwellMs(void) const;

    EngineConfig config_;
    OffscreenState state_;
    CgroupCalibrator calibrator_;

    int64_t pendingSinceMs_;   // Timestamp when entering PENDING_OFF (0 otherwise)
    int64_t lastInputActiveMs_; // Timestamp of last input activity (touch or btn)
};
