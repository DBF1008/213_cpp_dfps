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

#include <algorithm>

// EMA-based auto-calibrator for the restricted cgroup task count threshold.
//
// During normal screen-on operation, the restricted cgroup holds a baseline
// number of processes.  When the screen turns off, many more processes migrate
// in.  Instead of hardcoding a magic threshold (e.g. 10), we learn the baseline
// per-device and derive hi/lo thresholds with a hysteresis band.
//
// Usage:
//   - Call ObserveOnScreen(count) on every cgroup.re.list update while screen is ON
//   - Call Freeze() when entering offscreen (stop updating baseline)
//   - Call Unfreeze() when returning to onscreen (resume learning)
class CgroupCalibrator {
public:
    struct Config {
        float emaAlpha = 0.05f;   // EMA smoothing factor (~20 samples to converge)
        int warmupSamples = 30;   // Samples before calibration is trusted
        int hiMargin = 8;         // Threshold above baseline for ON -> PENDING_OFF
        int loMargin = 3;         // Threshold above baseline for OFF -> ON
        int absoluteMin = 3;      // Floor for learned baseline
        int absoluteMax = 50;     // Ceiling for learned baseline
    };

    CgroupCalibrator(void);
    explicit CgroupCalibrator(Config cfg);

    // Feed a restricted-cgroup PID count observed during screen-on.
    // Updates the EMA baseline.  Ignored while frozen.
    void ObserveOnScreen(int restrictedCount);

    // Freeze the baseline (call when entering offscreen to prevent contamination).
    void Freeze(void);

    // Resume learning (call when returning to onscreen).
    void Unfreeze(void);

    // True once warmupSamples have been collected.
    bool IsCalibrated(void) const;

    // Upper threshold: baseline + hiMargin.  Used for ON -> PENDING_OFF transition.
    int GetHiThreshold(void) const;

    // Lower threshold: baseline + loMargin.  Used for OFF -> ON transition.
    int GetLoThreshold(void) const;

    // Current EMA baseline (clamped to [absoluteMin, absoluteMax]).
    int GetBaseline(void) const;

    // Number of samples collected so far.
    int GetSampleCount(void) const;

    // True while baseline hasn't converged yet.
    bool IsWarmingUp(void) const;

private:
    int ClampBaseline(float raw) const;

    Config config_;
    float emaBaseline_;
    int sampleCount_;
    bool frozen_;
};
