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

#include "utils/screen_state.h"
#include <cstdint>

// OffscreenPolicy is the pure decision core that decides whether the screen is off.
//
// Motivation: deciding "offscreen" purely from the `restricted` cpuset task count crossing a
// fixed threshold is unreliable across ROMs/devices -- the threshold may not fit the device,
// and on devices without a `restricted` cpuset the signal never arrives at all. This policy
// fuses three signals so a misfitting threshold (or a missing cgroup) can no longer pin the
// refresh-rate controller to the wrong rule:
//
//   * restricted cgroup task count -- a cheap, frequent *hint* (OnRestrictedCount)
//   * the authoritative system screen state -- the source of truth (OnAuthoritative)
//   * the existing input event stream -- a corroborating veto/proxy (OnInput)
//
// Strategy: "heuristic proposes, authority confirms, input vetoes".
//   - The cheap cgroup hint and input never publish a verdict directly; they only *request*
//     an authoritative query. OnAuthoritative is the sole writer of the published verdict.
//     This keeps a single, well-ordered source of truth even though the inputs arrive on
//     several threads.
//   - The threshold is *learned* from authoritative feedback (self-calibrating) instead of
//     hardcoded, and is reset whenever the system contradicts the learned model. Until the
//     count is proven to separate on/off cleanly, the heuristic is "unreliable" and we always
//     defer to authority.
//   - A safety-net tick keeps re-confirming on degraded devices (no usable cgroup signal),
//     so a stuck verdict eventually self-corrects even with zero cgroup events.
//   - If the authority itself is unavailable (kUnknown), input is used as a last-resort
//     bidirectional proxy: recent input => on, prolonged silence => off.
//
// The class is intentionally free of any I/O, Android, threading or framework dependency: it
// is a value-like state machine driven entirely by explicit timestamps, so it can be unit
// tested deterministically on the host. All side effects are described by the returned
// OffscreenAction and performed by the caller (see modules/offscreen_monitor.cpp). The caller
// is responsible for serializing calls (e.g. behind a mutex).

struct OffscreenAction {
    // Publish `offscreenValue` on the "offscreen.state" topic.
    bool publish = false;
    bool offscreenValue = false;
    // Run the authoritative screen-state query (expensive) and feed the result back via
    // OnAuthoritative(). The policy has already applied rate-limiting / in-flight gating, so
    // the caller should honor this verbatim.
    bool queryAuthoritative = false;
    // If > 0, (re)arm the safety-net tick to fire again after this many microseconds.
    int64_t rearmTickUs = 0;
};

class OffscreenPolicy {
public:
    OffscreenPolicy();

    // The `restricted` cgroup task count changed (cgroup.re.list). `count` is the number of
    // tasks; `nowUs` is a monotonic timestamp in microseconds.
    OffscreenAction OnRestrictedCount(int count, int64_t nowUs);

    // A user input edge occurred (input.touch / input.btn). Used as a corroborating signal:
    // input while we believe the screen is off is a strong contradiction.
    OffscreenAction OnInput(int64_t nowUs);

    // Result of the authoritative screen-state query requested earlier. This is the only
    // method that may publish a verdict.
    OffscreenAction OnAuthoritative(ScreenState state, int64_t nowUs);

    // The safety-net tick fired. Keeps degraded devices (no usable cgroup signal) honest and
    // re-confirms healthy devices at a slow heartbeat. Returns the next tick delay.
    OffscreenAction OnTick(int64_t nowUs);

    // --- accessors (for tests / diagnostics) ---
    bool offscreen(void) const { return offscreen_; }
    bool reliable(void) const { return reliable_; }
    int threshold(void) const { return threshold_; }

private:
    void EnsureStart(int64_t nowUs);
    // Returns true if an authoritative query should be issued now. Sets the in-flight flag.
    // `bypassRateLimit` skips the min-interval check (for strong contradictions like input
    // while offscreen) but still respects the in-flight flag so queries cannot stack.
    bool RequestQuery(int64_t nowUs, bool bypassRateLimit);
    // Fold the latest count (lastCount_) into the calibration bands given a conclusive
    // authoritative screen state, then recompute the threshold / reliability.
    void UpdateCalibration(bool screenOn);
    void RecomputeReliability(void);

    // Published verdict (false == screen on / interactive). Mirrors the boot value the
    // consumer assumes.
    bool offscreen_;

    // --- self-calibrating threshold ---
    int threshold_;     // effective task-count threshold; learned, falls back to default
    bool reliable_;     // whether the cgroup count is trusted to discriminate on this device
    bool hasOn_;        // a conclusive on-sample has been recorded
    bool hasOff_;       // a conclusive off-sample has been recorded
    int maxOnCount_;    // largest restricted count seen while authority reported ON
    int minOffCount_;   // smallest restricted count seen while authority reported OFF

    // --- latest cgroup hint ---
    bool hasCount_;
    int lastCount_;

    // --- timestamps (microseconds, monotonic) ---
    int64_t startUs_;       // first timestamp observed (baseline for the silence fallback)
    int64_t lastInputUs_;   // last input edge
    int64_t lastActivityUs_; // last input or cgroup event (drives degraded poll cadence)
    int64_t lastQueryUs_;   // last authoritative query issued (rate-limit anchor)
    bool queryInFlight_;    // an authoritative query has been requested but not yet answered
};
