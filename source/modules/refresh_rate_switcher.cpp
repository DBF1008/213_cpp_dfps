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

#include "refresh_rate_switcher.h"
#include <cmath>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

namespace {

// How long to let a refresh-rate change settle, and how many times to re-read
// the device before concluding the switch did or did not take effect.
constexpr int SETTLE_TRIES = 4;
constexpr int SETTLE_MS = 25;
constexpr double RATE_TOL_HZ = 1.5;
// Consecutive unverified switches on a backend before we warn the operator once.
constexpr int DEGRADE_THRESHOLD = 3;

const char *BackendName(RrBackend backend) {
    return backend == RrBackend::PeakRefreshRate ? "PEAK_REFRESH_RATE" : "SurfaceFlinger backdoor";
}

RrBackend OtherBackend(RrBackend backend) {
    return backend == RrBackend::PeakRefreshRate ? RrBackend::SurfaceflingerBackdoor : RrBackend::PeakRefreshRate;
}

} // namespace

RefreshRateSwitcher::RefreshRateSwitcher(RrBackend primary, RrPlatform platform)
    : primary_(primary),
      active_(primary),
      plat_(std::move(platform)),
      haveConfirmed_(false),
      lastConfirmedValue_(0),
      lastConfidence_(RrConfidence::None),
      lastObservedHz_(-1),
      peakFailStreak_(0),
      sfFailStreak_(0),
      peakDegraded_(false),
      sfDegraded_(false),
      modesCached_(false) {}

bool RefreshRateSwitcher::Degraded(RrBackend backend) const {
    return backend == RrBackend::PeakRefreshRate ? peakDegraded_ : sfDegraded_;
}

int RefreshRateSwitcher::FailStreak(RrBackend backend) const {
    return backend == RrBackend::PeakRefreshRate ? peakFailStreak_ : sfFailStreak_;
}

int &RefreshRateSwitcher::FailStreakRef(RrBackend backend) {
    return backend == RrBackend::PeakRefreshRate ? peakFailStreak_ : sfFailStreak_;
}

bool &RefreshRateSwitcher::DegradedRef(RrBackend backend) {
    return backend == RrBackend::PeakRefreshRate ? peakDegraded_ : sfDegraded_;
}

const std::vector<DisplayMode> &RefreshRateSwitcher::ConfidentModes(void) {
    if (!modesCached_) {
        modesCached_ = true;
        std::vector<DisplayMode> modes;
        if (plat_.queryModes) {
            modes = plat_.queryModes();
        }
        double hz = plat_.queryActiveHz ? plat_.queryActiveHz() : -1;
        if (ModesAreConfident(modes, hz)) {
            modesCache_ = std::move(modes);
        } else {
            modesCache_.clear();
        }
    }
    return modesCache_;
}

bool RefreshRateSwitcher::EncodeFor(RrBackend target, int ruleValue, int *outValue, double *outHz) {
    if (primary_ == RrBackend::PeakRefreshRate) {
        // ruleValue is a refresh rate in Hz.
        if (target == RrBackend::PeakRefreshRate) {
            *outValue = ruleValue;
            *outHz = ruleValue;
            return true;
        }
        // target is the SF backdoor: need a config index for this Hz.
        const auto &modes = ConfidentModes();
        if (modes.empty()) {
            return false;
        }
        int idx = NearestModeIndexForHz(modes, ruleValue);
        if (idx < 0) {
            return false;
        }
        *outValue = idx;
        *outHz = HzForIndex(modes, idx);
        return true;
    }

    // primary_ is the SF backdoor: ruleValue is a config index.
    if (target == RrBackend::SurfaceflingerBackdoor) {
        *outValue = ruleValue;
        const auto &modes = ConfidentModes(); // best-effort expected Hz for logging
        *outHz = modes.empty() ? -1.0 : HzForIndex(modes, ruleValue);
        return true;
    }
    // target is PEAK: need a Hz for this index.
    const auto &modes = ConfidentModes();
    if (modes.empty()) {
        return false;
    }
    double hz = HzForIndex(modes, ruleValue);
    if (hz <= 0) {
        return false;
    }
    *outValue = static_cast<int>(hz + 0.5); // PEAK uses an integer Hz
    *outHz = hz;
    return true;
}

RefreshRateSwitcher::Attempt RefreshRateSwitcher::VerifyAttempt(RrBackend backend, int valueInEncoding,
                                                               double expectedHz) {
    Attempt a;
    bool sawWrong = false;
    double lastHz = -1;
    for (int i = 0; i < SETTLE_TRIES; ++i) {
        if (plat_.sleepMs) {
            plat_.sleepMs(SETTLE_MS);
        }
        if (backend == RrBackend::SurfaceflingerBackdoor) {
            int idx = plat_.queryActiveConfigIdx ? plat_.queryActiveConfigIdx() : -1;
            if (idx < 0) {
                continue; // oracle unavailable this round
            }
            if (idx == valueInEncoding) {
                a.confidence = RrConfidence::Strong;
                const auto &modes = ConfidentModes();
                a.observedHz = modes.empty() ? expectedHz : HzForIndex(modes, valueInEncoding);
                return a;
            }
            sawWrong = true;
        } else {
            double hz = plat_.queryActiveHz ? plat_.queryActiveHz() : -1;
            lastHz = hz;
            if (hz <= 0) {
                continue;
            }
            if (std::fabs(hz - expectedHz) <= RATE_TOL_HZ) {
                a.confidence = RrConfidence::Strong;
                a.observedHz = hz;
                return a;
            }
            sawWrong = true;
        }
    }
    if (sawWrong) {
        // Positive evidence the switch did not take effect.
        a.confidence = RrConfidence::None;
        a.observedHz = (backend == RrBackend::PeakRefreshRate) ? lastHz : -1;
    } else {
        // The apply command succeeded but the device exposes nothing to read
        // back; trust it (best effort) rather than loop forever.
        a.confidence = RrConfidence::Weak;
        a.observedHz = expectedHz;
    }
    return a;
}

RefreshRateSwitcher::Attempt RefreshRateSwitcher::AttemptConcrete(RrBackend backend, int valueInEncoding,
                                                                 double expectedHz, bool force) {
    bool applied = false;
    std::string value = std::to_string(valueInEncoding);
    if (backend == RrBackend::PeakRefreshRate) {
        applied = plat_.applyPeak && plat_.applyPeak(value, force);
    } else {
        applied = plat_.applySf && plat_.applySf(value, force);
    }
    if (!applied) {
        return Attempt{}; // confidence None, observedHz -1
    }
    return VerifyAttempt(backend, valueInEncoding, expectedHz);
}

void RefreshRateSwitcher::NoteSuccess(RrBackend backend) { FailStreakRef(backend) = 0; }

void RefreshRateSwitcher::NoteFailure(RrBackend backend) {
    int &streak = FailStreakRef(backend);
    ++streak;
    if (streak >= DEGRADE_THRESHOLD && !DegradedRef(backend)) {
        DegradedRef(backend) = true;
        SPDLOG_ERROR("Refresh rate switch via {} keeps failing verification on this device ({} attempts); "
                     "consider switching backend (toggle /useSfBackdoor to use {}) or fixing the rule values",
                     BackendName(backend), streak, BackendName(OtherBackend(backend)));
    }
}

SwitchResult RefreshRateSwitcher::Switch(int ruleValue, bool force) {
    SwitchResult result;

    // No-op fast path: already confirmed at this value on the active backend.
    if (!force && haveConfirmed_ && ruleValue == lastConfirmedValue_) {
        result.changed = false;
        result.verified = true;
        result.confidence = lastConfidence_;
        result.observedHz = lastObservedHz_;
        result.usedBackend = active_;
        return result;
    }
    result.changed = true;

    auto commit = [&](RrBackend backend, const Attempt &a) {
        active_ = backend;
        haveConfirmed_ = true;
        lastConfirmedValue_ = ruleValue;
        lastConfidence_ = a.confidence;
        lastObservedHz_ = a.observedHz;
        NoteSuccess(backend);
        result.verified = true;
        result.confidence = a.confidence;
        result.observedHz = a.observedHz;
        result.usedBackend = backend;
    };

    // 1) Attempt on the active backend.
    int value = 0;
    double expectedHz = -1;
    if (EncodeFor(active_, ruleValue, &value, &expectedHz)) {
        Attempt a = AttemptConcrete(active_, value, expectedHz, false);
        if (a.Succeeded()) {
            commit(active_, a);
            return result;
        }
        // 2) Escalate: retry the active backend with force.
        Attempt forced = AttemptConcrete(active_, value, expectedHz, true);
        if (forced.Succeeded()) {
            SPDLOG_DEBUG("Refresh rate {} confirmed on {} only after a forced retry", ruleValue,
                         BackendName(active_));
            commit(active_, forced);
            return result;
        }
    }

    // 3) Safe cross-backend fallback (only with a confident mode table for a
    //    safe value translation).
    RrBackend other = OtherBackend(active_);
    int otherValue = 0;
    double otherHz = -1;
    if (EncodeFor(other, ruleValue, &otherValue, &otherHz)) {
        Attempt b = AttemptConcrete(other, otherValue, otherHz, true);
        if (b.Succeeded()) {
            SPDLOG_WARN("Refresh rate {} unverified via {}; fell back to {} (value {})", ruleValue,
                        BackendName(active_), BackendName(other), otherValue);
            NoteFailure(active_); // the previously active backend failed here
            commit(other, b);     // commit switches active_ to `other`
            return result;
        }
    }

    // 4) Degrade: do not pretend success.
    NoteFailure(active_);
    haveConfirmed_ = false;
    result.verified = false;
    result.confidence = RrConfidence::None;
    result.usedBackend = active_;
    SPDLOG_WARN("Refresh rate switch to {} could not be verified on {}; reported rate left unchanged", ruleValue,
                BackendName(active_));
    return result;
}
