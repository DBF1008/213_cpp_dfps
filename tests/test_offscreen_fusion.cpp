/*
 * Regression tests for OffscreenStateFusion.
 *
 * Covers the full state machine: ON → PENDING_OFF → OFF, hard overrides,
 * hysteresis, warmup behavior, degraded mode, and anti-flapping.
 */

#include "test_framework.h"
#include "modules/offscreen_state_fusion.h"

// ---- Test helpers ----

// Build a FusionSignals with sensible defaults for testing.
// Default: kernel=UNKNOWN, no input, cgroup=0, time=0.
static FusionSignals MakeSignals(int cgroup = 0, DisplayState kernel = DisplayState::UNKNOWN,
                                  bool touch = false, bool btn = false, int64_t nowMs = 0) {
    FusionSignals s{};
    s.restrictedCgroupCount = cgroup;
    s.kernelDisplay = kernel;
    s.touchActive = touch;
    s.buttonActive = btn;
    s.nowMs = nowMs;
    return s;
}

// Pre-calibrate the engine so it's past the warmup period.
// Feeds `baselineCount` samples at the given count value.
static void PreCalibrate(OffscreenStateFusion &engine, int baselineCount, int samples = 50) {
    for (int i = 0; i < samples; i++) {
        auto s = MakeSignals(baselineCount, DisplayState::ON, false, false, i * 1000);
        engine.Update(s);
    }
}

// ---- Initial state tests ----

TEST(fusion_initial_state_is_ON) {
    OffscreenStateFusion engine;
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

TEST(fusion_starts_uncalibrated) {
    OffscreenStateFusion engine;
    ASSERT_FALSE(engine.IsCalibrated());
}

// ---- Hard override: touch ----

TEST(fusion_touch_while_ON_stays_ON) {
    OffscreenStateFusion engine;
    auto s = MakeSignals(0, DisplayState::UNKNOWN, true, false, 1000);
    ASSERT_EQ(engine.Update(s), OffscreenState::ON);
}

TEST(fusion_touch_while_PENDING_returns_to_ON) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Trigger PENDING_OFF: cgroup high, kernel unknown, input quiet
    auto s = MakeSignals(20, DisplayState::UNKNOWN, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Touch: should immediately return to ON
    s = MakeSignals(20, DisplayState::UNKNOWN, true, false, 10100);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

TEST(fusion_touch_while_OFF_returns_to_ON) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 1000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Drive to OFF
    int64_t t = 10000;
    auto s = MakeSignals(20, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s); // -> PENDING_OFF
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    t += 2000; // past dwell
    s = MakeSignals(20, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s); // -> OFF
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);

    // Touch: immediate return to ON
    t += 100;
    s = MakeSignals(20, DisplayState::UNKNOWN, true, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- Hard override: button ----

TEST(fusion_button_while_PENDING_returns_to_ON) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    auto s = MakeSignals(20, DisplayState::UNKNOWN, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    s = MakeSignals(20, DisplayState::UNKNOWN, false, true, 10100);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- Hard override: kernel display ON ----

TEST(fusion_kernel_ON_overrides_cgroup) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // High cgroup but kernel says ON — should stay ON
    auto s = MakeSignals(50, DisplayState::ON, false, false, 10000);
    ASSERT_EQ(engine.Update(s), OffscreenState::ON);
}

TEST(fusion_kernel_ON_while_PENDING_returns_to_ON) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Enter PENDING_OFF
    auto s = MakeSignals(20, DisplayState::UNKNOWN, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Kernel confirms display ON
    s = MakeSignals(20, DisplayState::ON, false, false, 10100);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- ON → PENDING_OFF transition ----

TEST(fusion_enters_PENDING_when_cgroup_high_no_input_kernel_off) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 1000;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Baseline ~5, hiThreshold = 5 + 8 = 13
    // cgroup=20 > 13, kernel=OFF, no input for long enough
    auto s = MakeSignals(20, DisplayState::OFF, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);
}

TEST(fusion_stays_ON_when_cgroup_below_threshold) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Baseline ~5, hiThreshold = 5 + 8 = 13
    // cgroup=10 < 13 — should stay ON
    auto s = MakeSignals(10, DisplayState::OFF, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

TEST(fusion_stays_ON_when_input_recently_active) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 2000;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // First, register touch activity
    auto s = MakeSignals(5, DisplayState::UNKNOWN, true, false, 8500);
    engine.Update(s);

    // Now cgroup is high but touch was active only 500ms ago (< inputQuietMs=2000)
    s = MakeSignals(20, DisplayState::OFF, false, false, 9000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- PENDING_OFF → OFF transition ----

TEST(fusion_confirms_OFF_after_dwell) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    int64_t t = 10000;
    auto s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s); // -> PENDING_OFF
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Still pending: not enough time elapsed
    t += 2000;
    s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Past dwell: confirm OFF
    t += 2000;
    s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);
}

// ---- PENDING_OFF → ON abort ----

TEST(fusion_abort_PENDING_when_cgroup_drops_below_lo) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    cfg.calibConfig.loMargin = 3;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Enter PENDING_OFF
    int64_t t = 10000;
    auto s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Cgroup drops below loThreshold (5 + 3 = 8): cgroup=2 < 8
    t += 500;
    s = MakeSignals(2, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- OFF → ON transition ----

TEST(fusion_returns_ON_from_OFF_when_cgroup_drops) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 1000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Drive to OFF
    int64_t t = 10000;
    auto s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s);
    t += 2000;
    s = MakeSignals(20, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);

    // System wakes up: processes leave restricted cgroup
    t += 1000;
    s = MakeSignals(2, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- Hysteresis anti-flapping ----

TEST(fusion_hysteresis_prevents_flapping) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.calibConfig.hiMargin = 8;
    cfg.calibConfig.loMargin = 3;
    cfg.pendingDwellMs = 1000;
    cfg.inputQuietMs = 200;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Baseline=5, hiThreshold=13, loThreshold=8
    int64_t t = 10000;

    // cgroup=14 > hi=13: enter PENDING_OFF
    auto s = MakeSignals(14, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Confirm OFF after dwell
    t += 2000;
    s = MakeSignals(14, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);

    // cgroup=10: between lo(8) and hi(13) — should NOT trigger return to ON
    // (10 is not < loThreshold=8)
    t += 500;
    s = MakeSignals(10, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);

    // cgroup=7 < lo=8: NOW return to ON
    t += 500;
    s = MakeSignals(7, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}

// ---- Degraded mode: kernel unavailable ----

TEST(fusion_degraded_mode_kernel_unknown_still_works) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 1000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // kernel=UNKNOWN throughout — should still work with cgroup alone
    int64_t t = 10000;
    auto s = MakeSignals(20, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    t += 2000;
    s = MakeSignals(20, DisplayState::UNKNOWN, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);
}

// ---- Warmup behavior ----

TEST(fusion_warmup_uses_extended_dwell) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 100; // very long warmup
    cfg.pendingDwellMs = 1000;
    cfg.inputQuietMs = 100;
    OffscreenStateFusion engine(cfg);

    // Don't pre-calibrate — we want to test warmup behavior
    int64_t t = 10000;
    auto s = MakeSignals(30, DisplayState::OFF, false, false, t);
    engine.Update(s);

    // Should be PENDING_OFF (or ON if threshold is too high during warmup)
    // During warmup, baseline defaults to absoluteMax/2 = 25, hiThreshold = 25+8 = 33
    // cgroup=30 < 33, so might not even enter PENDING
    // Let's use a higher cgroup count
    s = MakeSignals(40, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // During warmup, effective dwell = 2x = 2000ms
    // At 1500ms: still pending
    t += 1500;
    s = MakeSignals(40, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // At 2500ms: past 2x dwell (2000ms), confirm OFF
    t += 1000;
    s = MakeSignals(40, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);
}

// ---- Simultaneous conflicting signals ----

TEST(fusion_touch_wins_over_high_cgroup) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Touch active + very high cgroup + kernel OFF → touch wins → ON
    auto s = MakeSignals(100, DisplayState::OFF, true, false, 10000);
    ASSERT_EQ(engine.Update(s), OffscreenState::ON);
}

TEST(fusion_kernel_ON_wins_over_everything) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    // Kernel ON + high cgroup + no input → kernel wins → ON
    auto s = MakeSignals(100, DisplayState::ON, false, false, 10000);
    ASSERT_EQ(engine.Update(s), OffscreenState::ON);
}

// ---- Safety: all signals unavailable ----

TEST(fusion_all_signals_unavailable_stays_ON) {
    OffscreenStateFusion engine;

    // kernel=UNKNOWN, cgroup=0, no input — should stay ON (safe default)
    auto s = MakeSignals(0, DisplayState::UNKNOWN, false, false, 1000);
    ASSERT_EQ(engine.Update(s), OffscreenState::ON);
}

// ---- Rapid oscillation protection ----

TEST(fusion_rapid_oscillation_no_flapping) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    int64_t t = 10000;

    // Oscillate cgroup around threshold every 500ms
    for (int i = 0; i < 10; i++) {
        int cgroup = (i % 2 == 0) ? 20 : 5; // alternates above hi and below lo
        auto s = MakeSignals(cgroup, DisplayState::OFF, false, false, t);
        auto state = engine.Update(s);

        // Should never reach OFF because the dwell is 3000ms and we toggle every 500ms
        // Each time cgroup drops, PENDING resets to ON
        if (cgroup == 5) {
            ASSERT_EQ(state, OffscreenState::ON);
        }
        t += 500;
    }

    // After all the oscillation, we should NOT be in OFF state
    ASSERT_NE(engine.GetState(), OffscreenState::OFF);
}

// ---- PENDING_OFF timestamp tracking ----

TEST(fusion_pending_since_is_tracked) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 3000;
    cfg.inputQuietMs = 500;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    ASSERT_EQ(engine.GetPendingSinceMs(), 0);

    auto s = MakeSignals(20, DisplayState::OFF, false, false, 10000);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);
    ASSERT_EQ(engine.GetPendingSinceMs(), 10000);

    // Abort back to ON
    s = MakeSignals(2, DisplayState::UNKNOWN, false, false, 10500);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
    ASSERT_EQ(engine.GetPendingSinceMs(), 0);
}

// ---- Full lifecycle: screen on → off → on ----

TEST(fusion_full_lifecycle_on_off_on) {
    EngineConfig cfg;
    cfg.calibConfig.emaAlpha = 1.0f;
    cfg.calibConfig.warmupSamples = 1;
    cfg.pendingDwellMs = 2000;
    cfg.inputQuietMs = 1000;
    OffscreenStateFusion engine(cfg);

    PreCalibrate(engine, 5);

    int64_t t = 10000;

    // Phase 1: Screen ON, normal usage (touching)
    auto s = MakeSignals(5, DisplayState::ON, true, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);

    // Phase 2: User stops touching, screen goes off
    // Kernel says OFF, cgroup jumps (system moves processes to restricted)
    t += 3000; // enough time for input quiet
    s = MakeSignals(25, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::PENDING_OFF);

    // Phase 3: Dwell expires → confirmed OFF
    t += 3000;
    s = MakeSignals(25, DisplayState::OFF, false, false, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::OFF);

    // Phase 4: User presses power button → screen turns on
    t += 5000;
    s = MakeSignals(5, DisplayState::ON, false, true, t);
    engine.Update(s);
    ASSERT_EQ(engine.GetState(), OffscreenState::ON);
}
