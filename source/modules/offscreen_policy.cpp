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

#include "offscreen_policy.h"
#include <algorithm>

namespace {

constexpr int64_t US_PER_MS = 1000;
constexpr int64_t US_PER_S = 1000 * 1000;

// Initial guess before the threshold is learned. Matches the legacy hardcoded value so behavior
// on a freshly started, not-yet-calibrated device is unchanged until authority weighs in.
constexpr int DEFAULT_THRESHOLD = 10;

// The off-band low end must clear the on-band high end by at least this margin before the cgroup
// count is trusted to discriminate. A margin (rather than strict >) avoids latching "reliable"
// on a razor-thin, noisy separation.
constexpr int RELIABLE_MARGIN = 2;

// Minimum spacing between authoritative queries. dumpsys is relatively expensive, and screen
// transitions are infrequent, so this bounds the query rate without hurting responsiveness.
constexpr int64_t AUTH_QUERY_MIN_INTERVAL_US = 2000 * US_PER_MS;

// Safety-net tick cadence. While the heuristic is not yet trusted (which includes devices with no
// usable `restricted` cgroup, since those never calibrate) we poll the authority; quick right
// after activity, backing off when idle. Once trusted, we only re-confirm at a slow heartbeat.
constexpr int64_t DEGRADED_FAST_US = 3 * US_PER_S;
constexpr int64_t DEGRADED_SLOW_US = 15 * US_PER_S;
constexpr int64_t HEARTBEAT_US = 60 * US_PER_S;
constexpr int64_t ACTIVE_WINDOW_US = 10 * US_PER_S;

// Degraded fallback windows, used ONLY when the authority is unavailable (kUnknown): recent input
// means the screen is certainly on; prolonged silence is taken as off. The silence window is
// deliberately long so a quiet-but-on screen (e.g. watching a video) is not dimmed prematurely.
constexpr int64_t INPUT_FRESH_US = 1 * US_PER_S;
constexpr int64_t INPUT_SILENCE_OFF_US = 90 * US_PER_S;

// Sentinel for "no such event yet". Uses INT64_MIN/2 (not INT64_MIN) so that `nowUs - TS_NEVER`
// stays within int64_t range for any realistic monotonic timestamp.
constexpr int64_t TS_NEVER = INT64_MIN / 2;

} // namespace

OffscreenPolicy::OffscreenPolicy()
    : offscreen_(false),
      threshold_(DEFAULT_THRESHOLD),
      reliable_(false),
      hasOn_(false),
      hasOff_(false),
      maxOnCount_(0),
      minOffCount_(0),
      hasCount_(false),
      lastCount_(0),
      startUs_(TS_NEVER),
      lastInputUs_(TS_NEVER),
      lastActivityUs_(TS_NEVER),
      lastQueryUs_(TS_NEVER),
      queryInFlight_(false) {}

void OffscreenPolicy::EnsureStart(int64_t nowUs) {
    if (startUs_ == TS_NEVER) {
        startUs_ = nowUs;
    }
}

bool OffscreenPolicy::RequestQuery(int64_t nowUs, bool bypassRateLimit) {
    if (queryInFlight_) {
        return false;
    }
    if (bypassRateLimit == false && (nowUs - lastQueryUs_) < AUTH_QUERY_MIN_INTERVAL_US) {
        return false;
    }
    queryInFlight_ = true;
    lastQueryUs_ = nowUs;
    return true;
}

void OffscreenPolicy::RecomputeReliability(void) {
    if (hasOn_ && hasOff_ && minOffCount_ > maxOnCount_ + RELIABLE_MARGIN) {
        reliable_ = true;
        threshold_ = (maxOnCount_ + minOffCount_) / 2;
    } else {
        reliable_ = false;
        threshold_ = DEFAULT_THRESHOLD;
    }
}

void OffscreenPolicy::UpdateCalibration(bool screenOn) {
    if (screenOn) {
        // Authority says ON but the count looks like a previously-seen OFF level: the learned
        // model no longer matches reality, so drop it and relearn from this sample.
        if (hasOff_ && lastCount_ >= minOffCount_) {
            hasOn_ = false;
            hasOff_ = false;
        }
        maxOnCount_ = hasOn_ ? std::max(maxOnCount_, lastCount_) : lastCount_;
        hasOn_ = true;
    } else {
        if (hasOn_ && lastCount_ <= maxOnCount_) {
            hasOn_ = false;
            hasOff_ = false;
        }
        minOffCount_ = hasOff_ ? std::min(minOffCount_, lastCount_) : lastCount_;
        hasOff_ = true;
    }
    RecomputeReliability();
}

OffscreenAction OffscreenPolicy::OnRestrictedCount(int count, int64_t nowUs) {
    EnsureStart(nowUs);
    lastCount_ = count;
    hasCount_ = true;
    lastActivityUs_ = nowUs;

    OffscreenAction action;
    bool candidate = count > threshold_;
    // Confirm with authority whenever the cheap hint suggests a transition, or whenever we don't
    // (yet) trust the hint on this device.
    if (candidate != offscreen_ || reliable_ == false) {
        action.queryAuthoritative = RequestQuery(nowUs, false);
    }
    return action;
}

OffscreenAction OffscreenPolicy::OnInput(int64_t nowUs) {
    EnsureStart(nowUs);
    lastInputUs_ = nowUs;
    lastActivityUs_ = nowUs;

    OffscreenAction action;
    // Input while we believe the screen is off is a strong contradiction (an off screen does not
    // produce touch/button edges) -- confirm immediately, bypassing the rate limit.
    if (offscreen_) {
        action.queryAuthoritative = RequestQuery(nowUs, true);
    }
    return action;
}

OffscreenAction OffscreenPolicy::OnAuthoritative(ScreenState state, int64_t nowUs) {
    EnsureStart(nowUs);
    queryInFlight_ = false;

    bool newOffscreen = offscreen_;
    if (state == ScreenState::kOn) {
        newOffscreen = false;
        if (hasCount_) {
            UpdateCalibration(true);
        }
    } else if (state == ScreenState::kOff) {
        newOffscreen = true;
        if (hasCount_) {
            UpdateCalibration(false);
        }
    } else {
        // Authority unavailable: fall back to the best remaining signal.
        if (reliable_ && hasCount_) {
            newOffscreen = lastCount_ > threshold_;
        } else {
            bool recentInput = (lastInputUs_ != TS_NEVER) && (nowUs - lastInputUs_ < INPUT_FRESH_US);
            int64_t silenceRef = (lastInputUs_ != TS_NEVER) ? lastInputUs_ : startUs_;
            bool prolongedSilence = (nowUs - silenceRef) > INPUT_SILENCE_OFF_US;
            if (recentInput) {
                newOffscreen = false;
            } else if (prolongedSilence) {
                newOffscreen = true;
            }
            // otherwise keep the current verdict (avoid flapping on no information)
        }
    }

    OffscreenAction action;
    if (newOffscreen != offscreen_) {
        offscreen_ = newOffscreen;
        action.publish = true;
        action.offscreenValue = newOffscreen;
    }
    return action;
}

OffscreenAction OffscreenPolicy::OnTick(int64_t nowUs) {
    EnsureStart(nowUs);

    OffscreenAction action;
    action.queryAuthoritative = RequestQuery(nowUs, false);

    if (reliable_) {
        // Trusted device: rely on cgroup events + input, just re-confirm occasionally.
        action.rearmTickUs = HEARTBEAT_US;
    } else {
        bool active = (nowUs - lastActivityUs_) < ACTIVE_WINDOW_US;
        action.rearmTickUs = active ? DEGRADED_FAST_US : DEGRADED_SLOW_US;
    }
    return action;
}
