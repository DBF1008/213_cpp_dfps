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

#include "utils/refresh_rate_probe.h"
#include <functional>
#include <string>
#include <vector>

// Which low-level interface a value targets.
//   PeakRefreshRate        -> value is a refresh rate in Hz (settings put pins
//                             min == peak == hz)
//   SurfaceflingerBackdoor -> value is a SurfaceFlinger config index
//                             (service call SurfaceFlinger 1035 setActiveConfig)
enum class RrBackend {
    PeakRefreshRate,
    SurfaceflingerBackdoor,
};

// How sure we are the requested rate is actually in effect.
//   None   -> not in effect (apply failed, or the oracle reported a wrong rate)
//   Weak   -> the apply command succeeded but the device exposes no readable
//             rate to confirm against (best effort)
//   Strong -> confirmed by reading the device's actual rate / active config
enum class RrConfidence {
    None,
    Weak,
    Strong,
};

// Every device side effect the switcher needs, injected so the policy below is
// unit testable on the host with no Android dependency. Production wiring is
// MakeAndroidPlatform() (defined in utils/misc_android.cpp).
struct RrPlatform {
    // Apply peak_refresh_rate, pinning min == peak == hz. Returns command-level
    // success (the required keys were accepted); ROM-private keys are applied
    // best-effort and never make this fail.
    std::function<bool(const std::string &hz, bool force)> applyPeak;
    // Apply the SurfaceFlinger backdoor setActiveConfig(index). Returns
    // command-level success.
    std::function<bool(const std::string &index, bool force)> applySf;
    // The current actual refresh rate in Hz (<=0 unknown).
    std::function<double(void)> queryActiveHz;
    // The current active SurfaceFlinger config index (<0 unknown).
    std::function<int(void)> queryActiveConfigIdx;
    // The display mode table (index -> Hz). Used only for safe cross-backend
    // translation; may be empty or low confidence.
    std::function<std::vector<DisplayMode>(void)> queryModes;
    // Sleep helper used to let a rate change settle before reading it back.
    // Injected so tests run instantly.
    std::function<void(int ms)> sleepMs;
};

struct SwitchResult {
    bool changed = false;        // a switch was attempted (false on a no-op)
    bool verified = false;       // we are willing to report success (Strong or Weak)
    RrConfidence confidence = RrConfidence::None;
    double observedHz = -1;      // measured actual rate, for logging (<=0 unknown)
    RrBackend usedBackend = RrBackend::PeakRefreshRate; // backend that served it
};

// Drives a single refresh-rate switch through:
//   attempt (active backend) -> verify -> escalate (force) -> safe cross-backend
//   fallback -> degrade (stop lying, warn once).
// The "rule value" passed to Switch() is always in the *primary* backend's
// encoding (Hz for PeakRefreshRate, config index for SurfaceflingerBackdoor),
// matching the validated config; the switcher translates it internally when a
// cross-backend fallback is in effect.
class RefreshRateSwitcher {
public:
    RefreshRateSwitcher(RrBackend primary, RrPlatform platform);

    SwitchResult Switch(int ruleValue, bool force);

    // Inspection hooks (used by tests and logging).
    RrBackend PrimaryBackend(void) const { return primary_; }
    RrBackend ActiveBackend(void) const { return active_; }
    bool Degraded(RrBackend backend) const;
    int FailStreak(RrBackend backend) const;

private:
    struct Attempt {
        RrConfidence confidence = RrConfidence::None;
        double observedHz = -1;
        bool Succeeded(void) const { return confidence != RrConfidence::None; }
    };

    // Apply `valueInEncoding` (already in `backend`'s encoding) then verify.
    Attempt AttemptConcrete(RrBackend backend, int valueInEncoding, double expectedHz, bool force);
    Attempt VerifyAttempt(RrBackend backend, int valueInEncoding, double expectedHz);

    // Translate the primary-encoded `ruleValue` into `target`'s encoding.
    // Returns false when the translation is not safe (no confident mode table).
    bool EncodeFor(RrBackend target, int ruleValue, int *outValue, double *outHz);

    const std::vector<DisplayMode> &ConfidentModes(void);
    int &FailStreakRef(RrBackend backend);
    bool &DegradedRef(RrBackend backend);
    void NoteFailure(RrBackend backend);
    void NoteSuccess(RrBackend backend);

    RrBackend primary_;
    RrBackend active_;
    RrPlatform plat_;

    bool haveConfirmed_;
    int lastConfirmedValue_;
    RrConfidence lastConfidence_;
    double lastObservedHz_;

    int peakFailStreak_;
    int sfFailStreak_;
    bool peakDegraded_;
    bool sfDegraded_;

    bool modesCached_;
    std::vector<DisplayMode> modesCache_;
};

// Build the production RrPlatform backed by Android settings/service/dumpsys
// calls. Defined in utils/misc_android.cpp (which owns the Android-only I/O);
// host unit tests construct their own RrPlatform instead.
RrPlatform MakeAndroidPlatform(void);
