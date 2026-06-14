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

#include "modules/refresh_rate_switcher.h"
#include "test_framework.h"
#include "utils/refresh_rate_probe.h"
#include <string>

// A programmable fake device backing an RrPlatform. Tunable flags let each test
// reproduce a specific real-world failure mode without touching Android.
struct FakeDevice {
    std::vector<DisplayMode> modes;

    double curHz = -1;     // last actually-effective refresh rate
    int curIdx = -1;       // last actually-effective config index

    bool peakApplyOk = true;            // applyPeak command-level result
    bool peakApplyNeedsForce = false;   // applyPeak only succeeds when forced
    bool peakEffective = true;          // a successful applyPeak really changes the rate
    bool sfApplyOk = true;
    bool sfApplyNeedsForce = false;
    bool sfEffective = true;

    bool hzReadable = true;  // queryActiveHz returns the rate (else "unknown")
    bool idxReadable = true; // queryActiveConfigIdx returns the index (else "unknown")

    int sleeps = 0;

    RrPlatform Platform() {
        RrPlatform p;
        p.applyPeak = [this](const std::string &hz, bool force) {
            bool ok = peakApplyOk && (force || !peakApplyNeedsForce);
            if (ok && peakEffective) {
                curHz = std::stod(hz);
            }
            return ok;
        };
        p.applySf = [this](const std::string &idx, bool force) {
            bool ok = sfApplyOk && (force || !sfApplyNeedsForce);
            if (ok && sfEffective) {
                curIdx = std::stoi(idx);
                double hz = HzForIndex(modes, curIdx);
                if (hz > 0) {
                    curHz = hz;
                }
            }
            return ok;
        };
        p.queryActiveHz = [this]() { return hzReadable ? curHz : -1.0; };
        p.queryActiveConfigIdx = [this]() { return idxReadable ? curIdx : -1; };
        p.queryModes = [this]() { return modes; };
        p.sleepMs = [this](int) { ++sleeps; };
        return p;
    }
};

static std::vector<DisplayMode> ConfidentTable() { return {{0, 60.0}, {1, 90.0}, {2, 120.0}}; }

// (a) primary backend confirms on the first attempt.
TEST(switch_peak_success_first_try) {
    FakeDevice dev;
    dev.curHz = 60;
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto r = sw.Switch(120, false);
    CHECK(r.changed);
    CHECK(r.verified);
    CHECK(r.confidence == RrConfidence::Strong);
    CHECK(r.usedBackend == RrBackend::PeakRefreshRate);
    CHECK_NEAR(dev.curHz, 120.0, 0.1);
    CHECK(dev.sleeps > 0); // the settle/verify loop ran
}

// (a2) SF backdoor confirms via the active-config-index oracle.
TEST(switch_sf_success_first_try) {
    FakeDevice dev;
    dev.modes = ConfidentTable();
    dev.curHz = 60; // matches mode 0 so the table is confident
    RefreshRateSwitcher sw(RrBackend::SurfaceflingerBackdoor, dev.Platform());

    auto r = sw.Switch(2, false); // config index 2 -> 120 Hz
    CHECK(r.verified);
    CHECK(r.confidence == RrConfidence::Strong);
    CHECK(r.usedBackend == RrBackend::SurfaceflingerBackdoor);
    CHECK_EQ(dev.curIdx, 2);
}

// (b) first attempt fails command-level, forced escalation succeeds.
TEST(switch_escalate_with_force) {
    FakeDevice dev;
    dev.curHz = 60;
    dev.peakApplyNeedsForce = true; // unforced apply is rejected
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto r = sw.Switch(120, false);
    CHECK(r.verified);
    CHECK(r.confidence == RrConfidence::Strong);
    CHECK(r.usedBackend == RrBackend::PeakRefreshRate);
    CHECK_NEAR(dev.curHz, 120.0, 0.1);
}

// (c) apply succeeds but the rate never changes -> safe cross-backend recovers.
TEST(switch_crossbackend_when_verify_fails) {
    FakeDevice dev;
    dev.modes = ConfidentTable();
    dev.curHz = 60;            // confident table (matches mode 0)
    dev.peakEffective = false; // PEAK "succeeds" but does nothing
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto r = sw.Switch(120, false);
    CHECK(r.verified);
    CHECK(r.usedBackend == RrBackend::SurfaceflingerBackdoor); // fell over to SF
    CHECK(sw.ActiveBackend() == RrBackend::SurfaceflingerBackdoor);
    CHECK_EQ(dev.curIdx, 2); // index for 120 Hz
    // The previously-active PEAK backend recorded one failure.
    CHECK_EQ(sw.FailStreak(RrBackend::PeakRefreshRate), 1);
}

// (d) verify fails AND no confident table -> honest failure, no false success.
TEST(switch_honest_failure_without_table) {
    FakeDevice dev;
    dev.modes = {{0, 60.0}, {2, 120.0}}; // gap -> not confident -> no translation
    dev.curHz = 60;
    dev.peakEffective = false;
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto r = sw.Switch(120, false);
    CHECK(!r.verified);
    CHECK(r.changed);
    CHECK(r.confidence == RrConfidence::None);
    CHECK(sw.ActiveBackend() == RrBackend::PeakRefreshRate); // did not switch
    CHECK_EQ(sw.FailStreak(RrBackend::PeakRefreshRate), 1);

    // A failed switch must not become a no-op: the next call retries.
    auto r2 = sw.Switch(120, false);
    CHECK(r2.changed);
    CHECK(!r2.verified);
}

// (weak) apply succeeds but the device exposes no readable rate -> trust it.
TEST(switch_weak_when_unreadable) {
    FakeDevice dev;
    dev.curHz = 60;
    dev.hzReadable = false; // nothing to confirm against
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto r = sw.Switch(90, false);
    CHECK(r.verified);
    CHECK(r.confidence == RrConfidence::Weak);
    CHECK(r.usedBackend == RrBackend::PeakRefreshRate);
}

// (f) repeated unverified switches trip the degrade warning exactly once.
TEST(switch_degrade_after_repeated_failures) {
    FakeDevice dev;
    dev.modes = {{0, 60.0}, {2, 120.0}}; // not confident -> stuck on PEAK
    dev.curHz = 60;
    dev.peakEffective = false;
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    CHECK(!sw.Degraded(RrBackend::PeakRefreshRate));
    sw.Switch(120, false);
    CHECK(!sw.Degraded(RrBackend::PeakRefreshRate)); // streak 1
    sw.Switch(120, true);
    CHECK(!sw.Degraded(RrBackend::PeakRefreshRate)); // streak 2
    sw.Switch(120, true);
    CHECK(sw.Degraded(RrBackend::PeakRefreshRate)); // streak 3 -> warned
    CHECK_EQ(sw.FailStreak(RrBackend::PeakRefreshRate), 3);
    sw.Switch(120, true);
    CHECK(sw.Degraded(RrBackend::PeakRefreshRate)); // stays degraded
    CHECK_EQ(sw.FailStreak(RrBackend::PeakRefreshRate), 4);
}

// (g) repeating a confirmed value is a no-op unless forced.
TEST(switch_noop_when_already_at_target) {
    FakeDevice dev;
    dev.curHz = 60;
    RefreshRateSwitcher sw(RrBackend::PeakRefreshRate, dev.Platform());

    auto first = sw.Switch(120, false);
    CHECK(first.changed);
    CHECK(first.verified);

    auto again = sw.Switch(120, false);
    CHECK(!again.changed); // no-op fast path
    CHECK(again.verified);

    auto forced = sw.Switch(120, true);
    CHECK(forced.changed); // force bypasses the no-op
    CHECK(forced.verified);
}

// (c-helper) ROM-private settings are best-effort: required keys decide success.
TEST(required_settings_accepted_helper) {
    // peak/min required & accepted, miui optional & rejected -> still ok.
    CHECK(RequiredSettingsAccepted({{true, true}, {true, true}, {false, false}}));
    // a required key rejected -> failure.
    CHECK(!RequiredSettingsAccepted({{true, false}, {true, true}, {false, true}}));
    // nothing applied -> failure.
    CHECK(!RequiredSettingsAccepted({}));
    // only optional keys accepted, required missing -> failure.
    CHECK(!RequiredSettingsAccepted({{true, false}, {false, true}}));
}
