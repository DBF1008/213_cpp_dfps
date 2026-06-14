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

#include "offscreen_state_fusion.h"

OffscreenStateFusion::OffscreenStateFusion(void) : OffscreenStateFusion(EngineConfig{}) {}

OffscreenStateFusion::OffscreenStateFusion(EngineConfig cfg)
    : config_(cfg),
      state_(OffscreenState::ON),
      calibrator_(cfg.calibConfig),
      pendingSinceMs_(0),
      lastInputActiveMs_(0) {}

OffscreenState OffscreenStateFusion::Update(const FusionSignals &s) {
    // ---- Track input activity ----
    if (s.touchActive || s.buttonActive) {
        lastInputActiveMs_ = s.nowMs;
    }

    // ---- Hard override: physical input or kernel display ON → immediate ON ----
    if (HasHardOverrideToOn(s)) {
        if (state_ != OffscreenState::ON) {
            calibrator_.Unfreeze();
        }
        state_ = OffscreenState::ON;
        pendingSinceMs_ = 0;

        // While screen is ON, feed calibrator with current cgroup count
        calibrator_.ObserveOnScreen(s.restrictedCgroupCount);
        return state_;
    }

    // ---- State-specific transitions ----
    switch (state_) {
        case OffscreenState::ON: {
            // Check conditions BEFORE updating the calibrator baseline.
            // The current cgroup count may be elevated (screen just turned off),
            // so we must compare against the learned baseline, not the live value.
            bool shouldTransition =
                CgroupExceedsHi(s) && s.kernelDisplay != DisplayState::ON && IsInputQuiet(s);

            if (shouldTransition) {
                state_ = OffscreenState::PENDING_OFF;
                pendingSinceMs_ = s.nowMs;
                // Do NOT update calibrator — this count reflects offscreen state
            } else {
                // Screen is confirmed ON, so this cgroup count is a valid baseline sample
                calibrator_.ObserveOnScreen(s.restrictedCgroupCount);
            }
            break;
        }

        case OffscreenState::PENDING_OFF: {
            // Abort pending transition if cgroup drops below lo threshold
            if (CgroupBelowLo(s)) {
                state_ = OffscreenState::ON;
                pendingSinceMs_ = 0;
                break;
            }

            // Check if dwell period has elapsed
            int64_t dwellMs = GetEffectiveDwellMs();
            if (s.nowMs - pendingSinceMs_ >= dwellMs) {
                // Confirm offscreen
                state_ = OffscreenState::OFF;
                pendingSinceMs_ = 0;
                calibrator_.Freeze();
            }
            break;
        }

        case OffscreenState::OFF: {
            // Return to ON if cgroup drops below lo threshold
            // (the system has moved processes back out of restricted cgroup)
            if (CgroupBelowLo(s)) {
                state_ = OffscreenState::ON;
                pendingSinceMs_ = 0;
                calibrator_.Unfreeze();
            }
            break;
        }
    }

    return state_;
}

OffscreenState OffscreenStateFusion::GetState(void) const { return state_; }

int OffscreenStateFusion::GetCalibratedBaseline(void) const { return calibrator_.GetBaseline(); }

bool OffscreenStateFusion::IsCalibrated(void) const { return calibrator_.IsCalibrated(); }

int64_t OffscreenStateFusion::GetPendingSinceMs(void) const { return pendingSinceMs_; }

bool OffscreenStateFusion::HasHardOverrideToOn(const FusionSignals &s) const {
    // Physical input: touching the screen means it must be on
    if (s.touchActive || s.buttonActive) {
        return true;
    }
    // Kernel display state: authoritative hardware signal
    if (s.kernelDisplay == DisplayState::ON) {
        return true;
    }
    return false;
}

bool OffscreenStateFusion::CgroupExceedsHi(const FusionSignals &s) const {
    return s.restrictedCgroupCount > calibrator_.GetHiThreshold();
}

bool OffscreenStateFusion::CgroupBelowLo(const FusionSignals &s) const {
    return s.restrictedCgroupCount < calibrator_.GetLoThreshold();
}

bool OffscreenStateFusion::IsInputQuiet(const FusionSignals &s) const {
    // If no input has ever been active, consider it quiet
    if (lastInputActiveMs_ == 0) {
        return true;
    }
    return (s.nowMs - lastInputActiveMs_) >= config_.inputQuietMs;
}

int64_t OffscreenStateFusion::GetEffectiveDwellMs(void) const {
    // During warmup, use 2x dwell to be more conservative
    if (calibrator_.IsWarmingUp()) {
        return config_.pendingDwellMs * 2;
    }
    // When kernel state is unknown (unavailable), extend dwell for safety
    // This reduces false positives on devices where cgroup alone is unreliable
    // (Note: caller should check kernelDisplay == UNKNOWN before relying on this)
    return config_.pendingDwellMs;
}
