/*
 * Regression tests for the DynamicFps refresh-rate backend fallback logic.
 *
 * Build:  clang++ -std=c++17 -Wall -D_GNU_SOURCE -Wno-unused-parameter \
 *           -Isource -Ithirdparty/spdlog \
 *           tests/test_dynamic_fps_fallback.cpp tests/stubs_platform.cpp \
 *           source/modules/dynamic_fps.cpp source/platform/module_base.cpp \
 *           -o dfps_tests -pthread
 * Run:    ./dfps_tests
 */

/* macOS doesn't have timer_t in <time.h>; provide a stub typedef */
#ifdef __APPLE__
#include <sys/types.h>
#ifndef _TIMER_T_DEFINED_
typedef unsigned long timer_t;
#define _TIMER_T_DEFINED_
#endif
#endif

#include "modules/dynamic_fps.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <functional>

/* --- test_hooks declared in stubs_platform.cpp --- */
namespace test_hooks {
extern std::function<bool(const std::string &, bool)> peakRefreshRateHook;
extern std::function<bool(const std::string &, bool)> sfBackdoorHook;
extern std::function<bool(void)> probePeakHook;
extern std::function<bool(void)> probeSfHook;
extern std::string lastNotifyContent;
extern int notifyWriteCount;
extern void reset();
}

/* --- helpers --- */

static const char *TEST_CONFIG_PATH = "/tmp/dfps_test_config.txt";
static const char *TEST_NOTIFY_PATH = "/tmp/dfps_test_notify.txt";

static void WriteConfig(const std::string &content) {
    std::ofstream f(TEST_CONFIG_PATH);
    f << content;
    f.close();
}

static int testsPassed = 0;
static int testsFailed = 0;

/*
 * Friend class that accesses DynamicFps private members for testing.
 */
class DynamicFpsFallbackTest {
public:
    static void test_primary_succeeds();
    static void test_primary_fails_fallback_succeeds();
    static void test_both_backends_fail();
    static void test_init_probe_fallback();
    static void test_force_switch();
    static void test_permanent_fallback_persists();

private:
    static void setupCommonHooks();
};

void DynamicFpsFallbackTest::setupCommonHooks() {
    test_hooks::reset();
}

/*
 * Test 1: Primary backend succeeds → curHz_ updated, no fallback
 */
void DynamicFpsFallbackTest::test_primary_succeeds() {
    setupCommonHooks();

    // Both probes succeed, primary = peak_refresh_rate (default, useSfBackdoor=0)
    test_hooks::probePeakHook = []() { return true; };
    test_hooks::probeSfHook = []() { return true; };
    test_hooks::peakRefreshRateHook = [](const std::string &, bool) { return true; };
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return true; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    // curHz_ starts as INT32_MAX, so any hz will trigger a switch
    dfps.SwitchRefreshRate(60);

    if (dfps.curHz_ != 60) {
        printf("FAIL: curHz_=%d, expected 60\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    if (dfps.useFallback_ != false) {
        printf("FAIL: useFallback_=%d, expected false\n", dfps.useFallback_);
        testsFailed++;
        return;
    }
    if (dfps.consecutiveFailures_ != 0) {
        printf("FAIL: consecutiveFailures_=%d, expected 0\n", dfps.consecutiveFailures_);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/*
 * Test 2: Primary fails, fallback succeeds → curHz_ updated, permanent switch
 */
void DynamicFpsFallbackTest::test_primary_fails_fallback_succeeds() {
    setupCommonHooks();

    test_hooks::probePeakHook = []() { return true; };
    test_hooks::probeSfHook = []() { return true; };

    // Primary (peak_refresh_rate) always fails
    test_hooks::peakRefreshRateHook = [](const std::string &, bool) { return false; };
    // Fallback (SF backdoor) always succeeds
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return true; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    dfps.SwitchRefreshRate(120);

    if (dfps.curHz_ != 120) {
        printf("FAIL: curHz_=%d, expected 120\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    // Should have permanently switched to fallback
    if (dfps.useFallback_ != true) {
        printf("FAIL: useFallback_=%d, expected true\n", dfps.useFallback_);
        testsFailed++;
        return;
    }
    if (dfps.consecutiveFailures_ != 0) {
        printf("FAIL: consecutiveFailures_=%d, expected 0\n", dfps.consecutiveFailures_);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/*
 * Test 3: Both backends fail → curHz_ NOT updated, notification NOT written
 */
void DynamicFpsFallbackTest::test_both_backends_fail() {
    setupCommonHooks();

    test_hooks::probePeakHook = []() { return false; };
    test_hooks::probeSfHook = []() { return false; };
    test_hooks::peakRefreshRateHook = [](const std::string &, bool) { return false; };
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return false; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    int oldWriteCount = test_hooks::notifyWriteCount;
    dfps.SwitchRefreshRate(90);

    // curHz_ should remain INT32_MAX (initial value)
    if (dfps.curHz_ != INT32_MAX) {
        printf("FAIL: curHz_=%d, expected INT32_MAX\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    // No notification should have been written for the failed switch
    if (test_hooks::notifyWriteCount != oldWriteCount) {
        printf("FAIL: notifyWriteCount=%d, expected %d\n",
               test_hooks::notifyWriteCount, oldWriteCount);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/*
 * Test 4: Init probing — primary probe fails, starts with fallback
 */
void DynamicFpsFallbackTest::test_init_probe_fallback() {
    setupCommonHooks();

    // Primary (peak_refresh_rate) probe fails
    test_hooks::probePeakHook = []() { return false; };
    // Fallback (SF backdoor) probe succeeds
    test_hooks::probeSfHook = []() { return true; };
    test_hooks::peakRefreshRateHook = [](const std::string &, bool) { return false; };
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return true; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    // After init, primary should have been swapped to SF backdoor
    // useFallback_ should be false (we're using the new primary, which was the old fallback)
    if (dfps.useFallback_ != false) {
        printf("FAIL: useFallback_=%d, expected false\n", dfps.useFallback_);
        testsFailed++;
        return;
    }
    // The primary backend should now be SF backdoor (was swapped)
    if (dfps.primaryBackendName_ != "SurfaceFlinger backdoor") {
        printf("FAIL: primaryBackendName_='%s', expected 'SurfaceFlinger backdoor'\n",
               dfps.primaryBackendName_.c_str());
        testsFailed++;
        return;
    }
    if (dfps.fallbackBackendName_ != "peak_refresh_rate") {
        printf("FAIL: fallbackBackendName_='%s', expected 'peak_refresh_rate'\n",
               dfps.fallbackBackendName_.c_str());
        testsFailed++;
        return;
    }

    // Switching should use SF backdoor (the new primary)
    dfps.SwitchRefreshRate(60);
    if (dfps.curHz_ != 60) {
        printf("FAIL: curHz_=%d, expected 60\n", dfps.curHz_);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/*
 * Test 5: Force switch bypasses hz==curHz_ dedup
 */
void DynamicFpsFallbackTest::test_force_switch() {
    setupCommonHooks();

    test_hooks::probePeakHook = []() { return true; };
    test_hooks::probeSfHook = []() { return true; };

    int peakCallCount = 0;
    test_hooks::peakRefreshRateHook = [&peakCallCount](const std::string &, bool) {
        peakCallCount++;
        return true;
    };
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return true; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    // First switch to 60
    dfps.SwitchRefreshRate(60);
    if (dfps.curHz_ != 60) {
        printf("FAIL: curHz_=%d, expected 60\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    if (peakCallCount != 1) {
        printf("FAIL: peakCallCount=%d, expected 1\n", peakCallCount);
        testsFailed++;
        return;
    }

    // Same hz without force → should NOT call backend again
    dfps.SwitchRefreshRate(60);
    if (peakCallCount != 1) {
        printf("FAIL: peakCallCount=%d after no-force dup, expected 1\n", peakCallCount);
        testsFailed++;
        return;
    }

    // Force switch with same hz → SHOULD call backend
    dfps.forceSwitch_ = true;
    dfps.SwitchRefreshRate(60);
    if (peakCallCount != 2) {
        printf("FAIL: peakCallCount=%d after force, expected 2\n", peakCallCount);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/*
 * Test 6: After permanent fallback, stays on fallback for subsequent calls
 */
void DynamicFpsFallbackTest::test_permanent_fallback_persists() {
    setupCommonHooks();

    test_hooks::probePeakHook = []() { return true; };
    test_hooks::probeSfHook = []() { return true; };
    test_hooks::peakRefreshRateHook = [](const std::string &, bool) { return false; };
    test_hooks::sfBackdoorHook = [](const std::string &, bool) { return true; };

    WriteConfig(
        "# test config\n"
        "- 60 60\n"
        "* 60 120\n"
    );

    DynamicFps dfps(TEST_CONFIG_PATH, TEST_NOTIFY_PATH);

    // First switch: primary fails, fallback succeeds → permanent switch
    dfps.SwitchRefreshRate(120);
    if (dfps.curHz_ != 120) {
        printf("FAIL: curHz_=%d, expected 120\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    if (dfps.useFallback_ != true) {
        printf("FAIL: useFallback_=%d, expected true\n", dfps.useFallback_);
        testsFailed++;
        return;
    }

    // Second switch: should use fallback directly (no primary retry)
    dfps.SwitchRefreshRate(60);
    if (dfps.curHz_ != 60) {
        printf("FAIL: curHz_=%d, expected 60\n", dfps.curHz_);
        testsFailed++;
        return;
    }
    if (dfps.useFallback_ != true) {
        printf("FAIL: useFallback_=%d after 2nd switch, expected true\n", dfps.useFallback_);
        testsFailed++;
        return;
    }
    if (dfps.consecutiveFailures_ != 0) {
        printf("FAIL: consecutiveFailures_=%d, expected 0\n", dfps.consecutiveFailures_);
        testsFailed++;
        return;
    }

    printf("PASS\n");
    testsPassed++;
}

/* --- main --- */

int main() {
    printf("Running DynamicFps fallback tests...\n\n");

    printf("  [1/6] primary_succeeds ... ");
    DynamicFpsFallbackTest::test_primary_succeeds();

    printf("  [2/6] primary_fails_fallback_succeeds ... ");
    DynamicFpsFallbackTest::test_primary_fails_fallback_succeeds();

    printf("  [3/6] both_backends_fail ... ");
    DynamicFpsFallbackTest::test_both_backends_fail();

    printf("  [4/6] init_probe_fallback ... ");
    DynamicFpsFallbackTest::test_init_probe_fallback();

    printf("  [5/6] force_switch ... ");
    DynamicFpsFallbackTest::test_force_switch();

    printf("  [6/6] permanent_fallback_persists ... ");
    DynamicFpsFallbackTest::test_permanent_fallback_persists();

    printf("\nResults: %d passed, %d failed\n", testsPassed, testsFailed);

    // Cleanup
    remove(TEST_CONFIG_PATH);
    remove(TEST_NOTIFY_PATH);

    return testsFailed > 0 ? 1 : 0;
}
