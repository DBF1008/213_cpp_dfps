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

// Host regression test for the pure OffscreenPolicy decision core.
//
// OffscreenPolicy has no Android / threading / framework dependency, so it can be compiled and
// run on the host. cmake is not required:
//
//   clang++ -std=c++17 -I source test/offscreen_policy_test.cpp \
//           source/modules/offscreen_policy.cpp -o /tmp/op_test && /tmp/op_test
//
// (or run test/run.sh). These cases pin the behavior that fixes the two device-dependent
// misjudgements this change targets:
//   FM1 "stuck foreground while off"  -- no usable `restricted` cgroup signal.
//   FM2 "offscreen while bright"      -- >threshold tasks parked in `restricted` while interactive.

#include "modules/offscreen_policy.h"
#include <cstdio>

namespace {

constexpr int64_t MS = 1000;
constexpr int64_t S = 1000 * 1000;

int g_checks = 0;
int g_failures = 0;

void Check(bool cond, const char *expr, const char *test, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL [%s:%d] %s\n", test, line, expr);
    }
}

#define CHECK(cond) Check((cond), #cond, testName, __LINE__)

// FM1: a device with no usable `restricted` cgroup never emits OnRestrictedCount. The safety-net
// tick must still drive authoritative queries, and authority must be able to flip the verdict in
// BOTH directions (the legacy code was stuck at false forever here).
void TestDegradedNoCgroupRescue(void) {
    const char *testName = "degraded_no_cgroup_rescue";
    OffscreenPolicy p;

    auto a = p.OnTick(1 * S);
    CHECK(a.queryAuthoritative == true); // first poll is allowed
    CHECK(a.rearmTickUs > 0);            // and the tick keeps itself alive
    CHECK(p.reliable() == false);        // never calibrated -> stays degraded

    auto b = p.OnAuthoritative(ScreenState::kOff, 1 * S + 50 * MS);
    CHECK(b.publish == true && b.offscreenValue == true);
    CHECK(p.offscreen() == true);

    auto c = p.OnTick(20 * S);
    CHECK(c.queryAuthoritative == true);
    auto d = p.OnAuthoritative(ScreenState::kOn, 20 * S + 50 * MS);
    CHECK(d.publish == true && d.offscreenValue == false);
    CHECK(p.offscreen() == false);
}

// FM1 worst case: cgroup silent AND the authority is also unavailable (kUnknown). Input becomes a
// bidirectional proxy -- prolonged silence => off, recent input => on -- so the verdict still
// self-corrects instead of latching wrong.
void TestDegradedAuthorityUnavailable(void) {
    const char *testName = "degraded_authority_unavailable";
    OffscreenPolicy p;

    p.OnTick(10 * S); // establishes the silence baseline at t = 10s
    auto u1 = p.OnAuthoritative(ScreenState::kUnknown, 10 * S + 50 * MS);
    CHECK(u1.publish == false); // not enough observed silence yet -> keep
    CHECK(p.offscreen() == false);

    p.OnTick(120 * S); // 110s of silence > 90s window
    auto u2 = p.OnAuthoritative(ScreenState::kUnknown, 120 * S + 50 * MS);
    CHECK(u2.publish == true && u2.offscreenValue == true); // prolonged silence -> off
    CHECK(p.offscreen() == true);

    auto in = p.OnInput(130 * S);
    CHECK(in.queryAuthoritative == true); // input while "off" forces an immediate re-check
    auto u3 = p.OnAuthoritative(ScreenState::kUnknown, 130 * S + 10 * MS);
    CHECK(u3.publish == true && u3.offscreenValue == false); // recent input -> on
    CHECK(p.offscreen() == false);

    auto t = p.OnTick(200 * S);
    CHECK(t.rearmTickUs > 0); // still polling while degraded
    CHECK(p.reliable() == false);
}

// FM2: at boot a device parks >threshold tasks in `restricted` while interactive. The cheap hint
// must NOT publish offscreen on its own; authority confirms the screen is on and the verdict
// stays "on".
void TestBrightWithManyRestrictedTasks(void) {
    const char *testName = "bright_with_many_restricted_tasks";
    OffscreenPolicy p;

    auto a = p.OnRestrictedCount(15, 1 * S); // 15 > default 10
    CHECK(a.queryAuthoritative == true);     // asks authority instead of trusting the count
    CHECK(a.publish == false);               // the hint never publishes
    CHECK(p.offscreen() == false);

    auto b = p.OnAuthoritative(ScreenState::kOn, 1 * S + 50 * MS);
    CHECK(b.publish == false);   // no change: screen really is on
    CHECK(p.offscreen() == false);
}

// The threshold is learned from authoritative feedback rather than hardcoded.
void TestAdaptiveThresholdConverges(void) {
    const char *testName = "adaptive_threshold_converges";
    OffscreenPolicy p;
    int64_t t = 1 * S;

    p.OnRestrictedCount(2, t);
    p.OnAuthoritative(ScreenState::kOn, t + 1);
    t += 5 * S;
    p.OnRestrictedCount(8, t);
    p.OnAuthoritative(ScreenState::kOn, t + 1);
    t += 5 * S;
    p.OnRestrictedCount(20, t);
    p.OnAuthoritative(ScreenState::kOff, t + 1);

    CHECK(p.reliable() == true);
    CHECK(p.threshold() == 14); // midpoint(maxOn=8, minOff=20)
}

// If the bands overlap (the count cannot discriminate on this device), the heuristic must stay
// untrusted and every transition must keep deferring to authority.
void TestOverlappingBandsStayUnreliable(void) {
    const char *testName = "overlapping_bands_stay_unreliable";
    OffscreenPolicy p;

    auto a1 = p.OnRestrictedCount(18, 1 * S);
    CHECK(a1.queryAuthoritative == true);
    p.OnAuthoritative(ScreenState::kOn, 1 * S + 1); // ON observed at count 18

    auto a2 = p.OnRestrictedCount(12, 6 * S);
    CHECK(a2.queryAuthoritative == true);            // still asks authority
    p.OnAuthoritative(ScreenState::kOff, 6 * S + 1); // OFF observed at count 12 < 18 -> contradiction

    CHECK(p.reliable() == false);
}

// Input is a hard veto: a touch while we believe the screen is off must trigger an immediate
// re-check (bypassing the rate limit), and authority then flips the verdict back to on.
void TestInputVetoExitsOffscreen(void) {
    const char *testName = "input_veto_exits_offscreen";
    OffscreenPolicy p;

    p.OnTick(1 * S);
    p.OnAuthoritative(ScreenState::kOff, 1 * S + 50 * MS);
    CHECK(p.offscreen() == true);

    auto in = p.OnInput(1 * S + 100 * MS); // within the 2s rate-limit window, but bypassed
    CHECK(in.queryAuthoritative == true);

    auto a = p.OnAuthoritative(ScreenState::kOn, 1 * S + 150 * MS);
    CHECK(a.publish == true && a.offscreenValue == false);
    CHECK(p.offscreen() == false);
}

// When the authority is unavailable but the heuristic IS trusted, the verdict follows the learned
// threshold applied to the latest count.
void TestUnknownFallbackUsesReliableHeuristic(void) {
    const char *testName = "unknown_fallback_uses_reliable_heuristic";
    OffscreenPolicy p;
    int64_t t = 1 * S;

    // Calibrate to reliable: maxOn=8, minOff=20 -> threshold 14, verdict currently off.
    p.OnRestrictedCount(8, t);
    p.OnAuthoritative(ScreenState::kOn, t + 1);
    t += 5 * S;
    p.OnRestrictedCount(20, t);
    p.OnAuthoritative(ScreenState::kOff, t + 1);
    CHECK(p.reliable() == true);
    CHECK(p.offscreen() == true);

    t += 5 * S;
    p.OnRestrictedCount(3, t); // below learned threshold
    auto a = p.OnAuthoritative(ScreenState::kUnknown, t + 1);
    CHECK(a.publish == true && a.offscreenValue == false); // -> on
    CHECK(p.offscreen() == false);

    t += 5 * S;
    p.OnRestrictedCount(30, t); // above learned threshold
    auto b = p.OnAuthoritative(ScreenState::kUnknown, t + 1);
    CHECK(b.publish == true && b.offscreenValue == true); // -> off
    CHECK(p.offscreen() == true);
}

// Authoritative queries are rate-limited for the cheap-hint path, but a strong contradiction
// (input while offscreen) may bypass the rate limit -- yet never stack while one is in flight.
void TestQueryRateLimitingAndBypass(void) {
    const char *testName = "query_rate_limiting_and_bypass";
    OffscreenPolicy p;

    auto a = p.OnRestrictedCount(15, 0);
    CHECK(a.queryAuthoritative == true); // first is allowed
    p.OnAuthoritative(ScreenState::kUnknown, 100 * MS); // clears in-flight; keeps verdict
    CHECK(p.offscreen() == false);

    auto b = p.OnRestrictedCount(15, 1 * S);
    CHECK(b.queryAuthoritative == false); // within 2s -> rate limited

    auto c = p.OnRestrictedCount(15, 3 * S);
    CHECK(c.queryAuthoritative == true); // >= 2s later -> allowed
    p.OnAuthoritative(ScreenState::kUnknown, 3 * S + 100 * MS); // clears in-flight

    // Force the verdict to offscreen so input becomes a contradiction.
    p.OnAuthoritative(ScreenState::kOff, 4 * S);
    CHECK(p.offscreen() == true);

    auto in1 = p.OnInput(4 * S + 100 * MS); // would be rate-limited, but bypasses
    CHECK(in1.queryAuthoritative == true);
    auto in2 = p.OnInput(4 * S + 150 * MS); // a query is now in flight -> must not stack
    CHECK(in2.queryAuthoritative == false);
}

} // namespace

int main(void) {
    TestDegradedNoCgroupRescue();
    TestDegradedAuthorityUnavailable();
    TestBrightWithManyRestrictedTasks();
    TestAdaptiveThresholdConverges();
    TestOverlappingBandsStayUnreliable();
    TestInputVetoExitsOffscreen();
    TestUnknownFallbackUsesReliableHeuristic();
    TestQueryRateLimitingAndBypass();

    std::printf("offscreen_policy_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
