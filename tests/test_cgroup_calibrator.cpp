/*
 * Regression tests for CgroupCalibrator.
 *
 * Tests EMA convergence, freeze/unfreeze, safety bounds, and warmup behavior.
 */

#include "test_framework.h"
#include "modules/cgroup_calibrator.h"

// ---- Helper: feed N identical samples ----

static void FeedSamples(CgroupCalibrator &cal, int count, int n) {
    for (int i = 0; i < n; i++) {
        cal.ObserveOnScreen(count);
    }
}

// ---- Tests ----

TEST(calibrator_initial_baseline_is_conservative) {
    CgroupCalibrator cal;
    // Default initial baseline = absoluteMax / 2 = 25
    int baseline = cal.GetBaseline();
    ASSERT_GE(baseline, 3);
    ASSERT_LE(baseline, 50);
}

TEST(calibrator_not_calibrated_before_warmup) {
    CgroupCalibrator cal;
    ASSERT_FALSE(cal.IsCalibrated());
    ASSERT_TRUE(cal.IsWarmingUp());
}

TEST(calibrator_calibrated_after_warmup_samples) {
    CgroupCalibrator::Config cfg;
    cfg.warmupSamples = 10;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 5, 10);
    ASSERT_TRUE(cal.IsCalibrated());
    ASSERT_FALSE(cal.IsWarmingUp());
}

TEST(calibrator_ema_converges_to_steady_state) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 0.1f;
    cfg.warmupSamples = 5;
    CgroupCalibrator cal(cfg);

    // Feed 200 samples of value 8 — baseline should converge to ~8
    FeedSamples(cal, 8, 200);
    ASSERT_TRUE(cal.IsCalibrated());
    int baseline = cal.GetBaseline();
    ASSERT_GE(baseline, 7);
    ASSERT_LE(baseline, 9);
}

TEST(calibrator_ema_converges_to_high_value) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 0.1f;
    cfg.warmupSamples = 5;
    cfg.absoluteMax = 100;
    CgroupCalibrator cal(cfg);

    // Feed 200 samples of value 40
    FeedSamples(cal, 40, 200);
    int baseline = cal.GetBaseline();
    ASSERT_GE(baseline, 39);
    ASSERT_LE(baseline, 41);
}

TEST(calibrator_hi_threshold_is_baseline_plus_margin) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f; // instant convergence
    cfg.warmupSamples = 1;
    cfg.hiMargin = 8;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    ASSERT_EQ(cal.GetBaseline(), 10);
    ASSERT_EQ(cal.GetHiThreshold(), 18); // 10 + 8
}

TEST(calibrator_lo_threshold_is_baseline_plus_margin) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    cfg.loMargin = 3;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    ASSERT_EQ(cal.GetBaseline(), 10);
    ASSERT_EQ(cal.GetLoThreshold(), 13); // 10 + 3
}

TEST(calibrator_freeze_stops_learning) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    ASSERT_EQ(cal.GetBaseline(), 10);

    cal.Freeze();
    FeedSamples(cal, 50, 100); // should be ignored
    ASSERT_EQ(cal.GetBaseline(), 10);
}

TEST(calibrator_unfreeze_resumes_learning) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    ASSERT_EQ(cal.GetBaseline(), 10);

    cal.Freeze();
    FeedSamples(cal, 50, 10);
    ASSERT_EQ(cal.GetBaseline(), 10);

    cal.Unfreeze();
    FeedSamples(cal, 30, 5);
    ASSERT_EQ(cal.GetBaseline(), 30);
}

TEST(calibrator_baseline_clamped_to_absolute_min) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    cfg.absoluteMin = 5;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 1, 5);
    ASSERT_GE(cal.GetBaseline(), cfg.absoluteMin);
}

TEST(calibrator_baseline_clamped_to_absolute_max) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    cfg.absoluteMax = 30;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 100, 5);
    ASSERT_LE(cal.GetBaseline(), cfg.absoluteMax);
}

TEST(calibrator_hysteresis_band_exists) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    cfg.warmupSamples = 1;
    cfg.hiMargin = 8;
    cfg.loMargin = 3;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    ASSERT_GT(cal.GetHiThreshold(), cal.GetLoThreshold());
    ASSERT_EQ(cal.GetHiThreshold() - cal.GetLoThreshold(), cfg.hiMargin - cfg.loMargin);
}

TEST(calibrator_sample_count_increments) {
    CgroupCalibrator cal;
    ASSERT_EQ(cal.GetSampleCount(), 0);
    cal.ObserveOnScreen(5);
    ASSERT_EQ(cal.GetSampleCount(), 1);
    cal.ObserveOnScreen(5);
    ASSERT_EQ(cal.GetSampleCount(), 2);
}

TEST(calibrator_frozen_does_not_change_baseline) {
    CgroupCalibrator::Config cfg;
    cfg.emaAlpha = 1.0f;
    CgroupCalibrator cal(cfg);

    FeedSamples(cal, 10, 5);
    int baselineBefore = cal.GetBaseline();

    cal.Freeze();
    cal.ObserveOnScreen(100);
    ASSERT_EQ(cal.GetBaseline(), baselineBefore);
}
