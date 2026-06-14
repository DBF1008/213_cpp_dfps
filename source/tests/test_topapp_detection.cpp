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

#include <gtest/gtest.h>
#include "utils/misc_android.h"

#include <cstdlib>
#include <string>
#include <vector>

// ============================================================
// ExtractPkgNameFromCmdline — Pure Function Tests
// ============================================================

class ExtractPkgNameTest : public ::testing::Test {};

// --- Valid package names ---

TEST_F(ExtractPkgNameTest, StandardPackageName) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm"), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, MiuiHome) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.miui.home"), "com.miui.home");
}

TEST_F(ExtractPkgNameTest, SingleDotPackage) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("a.b"), "a.b");
}

TEST_F(ExtractPkgNameTest, DeeplyNestedPackage) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.example.deep.nested.app"), "com.example.deep.nested.app");
}

TEST_F(ExtractPkgNameTest, SubprocessSuffixStripped) {
    // Android subprocess cmdlines: "com.tencent.mm:toolsmp" → "com.tencent.mm"
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm:toolsmp"), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, SubprocessSuffixRemote) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.miui.securitycenter.remote"), "com.miui.securitycenter.remote");
    // Note: ".remote" is part of the package name (no colon), not a subprocess
}

TEST_F(ExtractPkgNameTest, SubprocessSuffixPush) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm:push"), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, NullTerminatedWithArgs) {
    // Simulate real /proc/pid/cmdline: "com.app\0arg1\0arg2"
    std::string cmdline("com.tencent.mm\0-some-flag\0", 28);
    EXPECT_EQ(ExtractPkgNameFromCmdline(cmdline), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, TrailingNewline) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm\n"), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, TrailingWhitespace) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm  \n"), "com.tencent.mm");
}

TEST_F(ExtractPkgNameTest, TrailingCarriageReturn) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("com.tencent.mm\r\n"), "com.tencent.mm");
}

// --- Invalid inputs (should return empty) ---

TEST_F(ExtractPkgNameTest, EmptyString) {
    EXPECT_EQ(ExtractPkgNameFromCmdline(""), "");
}

TEST_F(ExtractPkgNameTest, NativeBinaryAbsolutePath) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("/system/bin/surfaceflinger"), "");
}

TEST_F(ExtractPkgNameTest, NativeBinaryRelativePath) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("./my_binary"), "");
}

TEST_F(ExtractPkgNameTest, BareNameNoDot_Zygote) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("zygote"), "");
}

TEST_F(ExtractPkgNameTest, BareNameNoDot_SystemServer) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("system_server"), "");
}

TEST_F(ExtractPkgNameTest, BareNameNoDot_Adbd) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("adbd"), "");
}

TEST_F(ExtractPkgNameTest, BareNameNoDot_Init) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("init"), "");
}

TEST_F(ExtractPkgNameTest, LeadingDot) {
    EXPECT_EQ(ExtractPkgNameFromCmdline(".hidden"), "");
}

TEST_F(ExtractPkgNameTest, OnlyWhitespace) {
    EXPECT_EQ(ExtractPkgNameFromCmdline("   \n"), "");
}

TEST_F(ExtractPkgNameTest, NullOnly) {
    std::string cmdline("\0\0\0", 3);
    EXPECT_EQ(ExtractPkgNameFromCmdline(cmdline), "");
}

TEST_F(ExtractPkgNameTest, NullThenNativePath) {
    std::string cmdline("\0/system/bin/sh", 14);
    EXPECT_EQ(ExtractPkgNameFromCmdline(cmdline), "");
}

TEST_F(ExtractPkgNameTest, NativeBinaryWithNullArgs) {
    std::string cmdline("/system/bin/sh\0-c\0ls\0", 20);
    EXPECT_EQ(ExtractPkgNameFromCmdline(cmdline), "");
}

// ============================================================
// Threshold Decision Tests
// Verify the debounce predicate from OnTopappList.
// ============================================================

class ThresholdDecisionTest : public ::testing::Test {
protected:
    // Mirror the constant from topapp_monitor.cpp
    static constexpr int TOP_TASK_NR_DIFF_MIN = 10;

    bool ShouldSkipDumpsys(int oldNr, int newNr) {
        return std::abs(newNr - oldNr) <= TOP_TASK_NR_DIFF_MIN;
    }
};

TEST_F(ThresholdDecisionTest, IdenticalCount_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(50, 50));
}

TEST_F(ThresholdDecisionTest, SmallIncrease_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(50, 55)); // |5| <= 10
}

TEST_F(ThresholdDecisionTest, SmallDecrease_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(55, 50)); // |5| <= 10
}

TEST_F(ThresholdDecisionTest, ExactThreshold_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(50, 60)); // |10| <= 10
}

TEST_F(ThresholdDecisionTest, ExactThresholdNegative_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(60, 50)); // |10| <= 10
}

TEST_F(ThresholdDecisionTest, JustOverThreshold_ShouldNotSkip) {
    EXPECT_FALSE(ShouldSkipDumpsys(50, 61)); // |11| > 10
}

TEST_F(ThresholdDecisionTest, JustOverThresholdNegative_ShouldNotSkip) {
    EXPECT_FALSE(ShouldSkipDumpsys(61, 50)); // |11| > 10
}

TEST_F(ThresholdDecisionTest, LargeChange_ShouldNotSkip) {
    EXPECT_FALSE(ShouldSkipDumpsys(10, 100));
}

TEST_F(ThresholdDecisionTest, ZeroToOne_ShouldSkip) {
    EXPECT_TRUE(ShouldSkipDumpsys(0, 1));
}

TEST_F(ThresholdDecisionTest, ZeroToLarge_ShouldNotSkip) {
    EXPECT_FALSE(ShouldSkipDumpsys(0, 50));
}

// ============================================================
// Detection Flow Simulation Tests
// Verify the three-layer detection logic:
//   1. Fast path (cmdline, ~1ms)
//   2. Threshold + dumpsys fallback (800ms)
//   3. Heartbeat (5s safety net)
// These simulate the decision logic without real I/O.
// ============================================================

class DetectionFlowTest : public ::testing::Test {
protected:
    static constexpr int TOP_TASK_NR_DIFF_MIN = 10;

    struct SimState {
        int topappNr = 0;
        std::string prevPkgName;
        std::string lastPublished;
        std::string publishSource; // "fast", "dumpsys", "heartbeat"
        int dumpsysScheduled = 0;
        int fastPathAttempts = 0;
    };

    // Simulates TryFastPathUpdate
    void SimFastPath(SimState &s, const std::string &cmdlineResult) {
        s.fastPathAttempts++;
        if (cmdlineResult.empty()) {
            return;
        }
        if (cmdlineResult == s.prevPkgName) {
            return;
        }
        s.prevPkgName = cmdlineResult;
        s.lastPublished = cmdlineResult;
        s.publishSource = "fast";
    }

    // Simulates threshold check + ScheduleDumpsysFallback
    void SimThresholdAndDumpsys(SimState &s, int newNr) {
        if (std::abs(newNr - s.topappNr) <= TOP_TASK_NR_DIFF_MIN) {
            return;
        }
        s.topappNr = newNr;
        s.dumpsysScheduled++;
    }

    // Simulates DoDumpsysUpdate (called after 800ms delay)
    void SimDumpsysResult(SimState &s, const std::string &dumpsysResult) {
        if (dumpsysResult.empty()) {
            return;
        }
        if (dumpsysResult == s.prevPkgName) {
            return; // dedup — fast path already set this
        }
        s.prevPkgName = dumpsysResult;
        s.lastPublished = dumpsysResult;
        s.publishSource = "dumpsys";
    }

    // Simulates HeartbeatCheck
    void SimHeartbeat(SimState &s, const std::string &dumpsysResult) {
        if (dumpsysResult.empty()) {
            return;
        }
        if (dumpsysResult == s.prevPkgName) {
            return;
        }
        s.prevPkgName = dumpsysResult;
        s.lastPublished = dumpsysResult;
        s.publishSource = "heartbeat";
    }
};

// THE KEY BUG FIX TEST: similar thread count, fast path catches it
TEST_F(DetectionFlowTest, SimilarThreadCount_FastPathCatchesIt) {
    SimState s;
    s.topappNr = 50;
    s.prevPkgName = "com.app.a";

    // App A (50 threads) → App B (55 threads): |5| <= 10, old code would miss
    SimFastPath(s, "com.app.b");
    SimThresholdAndDumpsys(s, 55);

    EXPECT_EQ(s.lastPublished, "com.app.b"); // FIXED: was stale before
    EXPECT_EQ(s.publishSource, "fast");
    EXPECT_EQ(s.dumpsysScheduled, 0);         // no dumpsys needed
}

TEST_F(DetectionFlowTest, LargeThreadChange_BothPathsWork) {
    SimState s;
    s.topappNr = 10;
    s.prevPkgName = "com.app.a";

    SimFastPath(s, "com.app.b");
    SimThresholdAndDumpsys(s, 100);

    EXPECT_EQ(s.lastPublished, "com.app.b");
    EXPECT_EQ(s.publishSource, "fast");
    EXPECT_EQ(s.dumpsysScheduled, 1); // dumpsys also scheduled as backup
}

TEST_F(DetectionFlowTest, FastPathFails_ThresholdPasses_DumpsysCatchesIt) {
    SimState s;
    s.topappNr = 10;
    s.prevPkgName = "com.app.a";

    SimFastPath(s, "");       // cmdline read failed (e.g., native process)
    SimThresholdAndDumpsys(s, 100);

    EXPECT_EQ(s.lastPublished, "");   // fast path didn't publish
    EXPECT_EQ(s.dumpsysScheduled, 1); // dumpsys will catch it

    // Simulate dumpsys completing after 800ms
    SimDumpsysResult(s, "com.app.b");
    EXPECT_EQ(s.lastPublished, "com.app.b");
    EXPECT_EQ(s.publishSource, "dumpsys");
}

TEST_F(DetectionFlowTest, FastPathFails_ThresholdFails_HeartbeatCatchesIt) {
    SimState s;
    s.topappNr = 50;
    s.prevPkgName = "com.app.a";

    SimFastPath(s, "");      // cmdline failed
    SimThresholdAndDumpsys(s, 55); // threshold blocks

    EXPECT_EQ(s.lastPublished, "");
    EXPECT_EQ(s.dumpsysScheduled, 0);

    // Heartbeat fires within 5 seconds
    SimHeartbeat(s, "com.app.b");
    EXPECT_EQ(s.lastPublished, "com.app.b");
    EXPECT_EQ(s.publishSource, "heartbeat");
}

TEST_F(DetectionFlowTest, SameApp_NoRedundantPublish) {
    SimState s;
    s.prevPkgName = "com.app.a";

    SimFastPath(s, "com.app.a"); // same app

    EXPECT_EQ(s.lastPublished, ""); // nothing published (dedup)
}

TEST_F(DetectionFlowTest, RapidSuccessiveSwitches) {
    SimState s;
    s.prevPkgName = "com.app.a";
    s.topappNr = 50;

    // Switch 1: A→B (large change)
    SimFastPath(s, "com.app.b");
    SimThresholdAndDumpsys(s, 100);
    EXPECT_EQ(s.lastPublished, "com.app.b");

    // Switch 2: B→C (small change, before dumpsys runs)
    SimFastPath(s, "com.app.c");
    SimThresholdAndDumpsys(s, 95); // |95-100| = 5 <= 10, threshold blocks
    EXPECT_EQ(s.lastPublished, "com.app.c"); // fast path caught it

    EXPECT_EQ(s.dumpsysScheduled, 1); // only one dumpsys from switch 1
}

TEST_F(DetectionFlowTest, DumpsysConfirmsFastPath_NoRedundantPublish) {
    SimState s;
    s.prevPkgName = "com.app.a";
    s.topappNr = 10;

    // Fast path detects new app
    SimFastPath(s, "com.app.b");
    EXPECT_EQ(s.lastPublished, "com.app.b");

    // Threshold passes, dumpsys scheduled
    SimThresholdAndDumpsys(s, 100);

    // Dumpsys runs after 800ms — same result as fast path
    SimDumpsysResult(s, "com.app.b");
    EXPECT_EQ(s.publishSource, "fast"); // still fast, dumpsys was redundant
}

TEST_F(DetectionFlowTest, GestureBackToHome) {
    SimState s;
    s.prevPkgName = "com.tencent.mm";
    s.topappNr = 60;

    // User swipes back to home screen — thread count similar
    SimFastPath(s, "com.miui.home");
    SimThresholdAndDumpsys(s, 55); // |5| <= 10, threshold blocks

    EXPECT_EQ(s.lastPublished, "com.miui.home");
    EXPECT_EQ(s.publishSource, "fast");
}

TEST_F(DetectionFlowTest, HeartbeatCorrectsStaleState) {
    SimState s;
    s.prevPkgName = "com.app.a";
    s.topappNr = 50;

    // Both fast path and threshold fail somehow
    SimFastPath(s, "");
    SimThresholdAndDumpsys(s, 55);
    EXPECT_EQ(s.lastPublished, "");

    // Heartbeat catches the missed switch
    SimHeartbeat(s, "com.app.b");
    EXPECT_EQ(s.lastPublished, "com.app.b");
    EXPECT_EQ(s.publishSource, "heartbeat");

    // Next heartbeat with same app — no redundant publish
    s.lastPublished = "";
    SimHeartbeat(s, "com.app.b");
    EXPECT_EQ(s.lastPublished, ""); // dedup worked
}
