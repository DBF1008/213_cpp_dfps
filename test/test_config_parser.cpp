/*
 * Regression tests for ParseConfigFile().
 * Build on host:  mkdir -p build && cd build && cmake ../test && make
 * Run:            ./runnable/config_parser_test
 */

#include "config_parser.h"
#include "utils/fmt_exception.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

// Helper: write content to a temp file, return its path.  Caller must remove().
static std::string WriteTempConfig(const char *content) {
    char path[] = "/tmp/dfps_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    write(fd, content, strlen(content));
    close(fd);
    return std::string(path);
}

#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        try {                                                                                                          \
            name();                                                                                                    \
            std::cout << "PASS" << std::endl;                                                                          \
            passed++;                                                                                                  \
        } catch (const std::exception &e) {                                                                            \
            std::cout << "FAIL (" << e.what() << ")" << std::endl;                                                     \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

// ---------- Test cases ----------

// 1. Valid full config — all fields populated
void TestValidFullConfig(void) {
    auto path = WriteTempConfig(
        "# comment\n"
        "/touchSlackMs 5000\n"
        "/enableMinBrightness 10\n"
        "/useSfBackdoor 0\n"
        "com.example.app 60 120\n"
        "- -1 -1\n"
        "* 60 120\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.hasUniversial == true);
    assert(cfg.hasOffscreen == true);
    assert(cfg.useSfBackdoor == false);
    assert(cfg.touchSlackMs == 5000);
    assert(cfg.enableMinBrightness == 10);
    assert(cfg.universial.idle == 60);
    assert(cfg.universial.active == 120);
    assert(cfg.offscreen.idle == -1);
    assert(cfg.offscreen.active == -1);
    assert(cfg.rules.count("com.example.app") == 1);
    assert(cfg.rules["com.example.app"].idle == 60);
    assert(cfg.rules["com.example.app"].active == 120);
}

// 2. Missing universal (*) rule — must throw
void TestMissingUniversalRule(void) {
    auto path = WriteTempConfig(
        "- -1 -1\n"
        "com.example.app 60 120\n");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &e) {
        threw = true;
        assert(std::string(e.what()).find("Default rule") != std::string::npos);
    }
    remove(path.c_str());
    assert(threw);
}

// 3. Missing offscreen (-) rule — must throw
void TestMissingOffscreenRule(void) {
    auto path = WriteTempConfig(
        "* 60 120\n"
        "com.example.app 60 120\n");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &e) {
        threw = true;
        assert(std::string(e.what()).find("Offscreen rule") != std::string::npos);
    }
    remove(path.c_str());
    assert(threw);
}

// 4. Non-existent config file — must throw
void TestNonExistentFile(void) {
    bool threw = false;
    try {
        ParseConfigFile("/tmp/dfps_no_such_file_12345.txt");
    } catch (const FmtException &e) {
        threw = true;
        assert(std::string(e.what()).find("Cannot open") != std::string::npos);
    }
    assert(threw);
}

// 5. Empty file — must throw (missing both required rules)
void TestEmptyFile(void) {
    auto path = WriteTempConfig("");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &) {
        threw = true;
    }
    remove(path.c_str());
    assert(threw);
}

// 6. Comments-only file — must throw
void TestCommentsOnlyFile(void) {
    auto path = WriteTempConfig(
        "# comment 1\n"
        "# comment 2\n"
        "\n");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &) {
        threw = true;
    }
    remove(path.c_str());
    assert(threw);
}

// 7. Malformed lines — skipped, valid lines still parsed
void TestMalformedLinesSkipped(void) {
    auto path = WriteTempConfig(
        "this line is garbage\n"
        "also_garbage\n"
        "* 60 120\n"
        "broken line with only one value 60\n"
        "- -1 -1\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.hasUniversial == true);
    assert(cfg.hasOffscreen == true);
    assert(cfg.universial.idle == 60);
    assert(cfg.universial.active == 120);
}

// 8. Unknown tunable — warning logged, no throw
void TestUnknownTunable(void) {
    auto path = WriteTempConfig(
        "/unknownTunable 42\n"
        "* 60 120\n"
        "- -1 -1\n");

    // Should NOT throw
    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.hasUniversial == true);
    assert(cfg.hasOffscreen == true);
}

// 9. Invalid rule: Hz mode (useSfBackdoor=0) with backdoor-range values (<20)
void TestInvalidRuleHzModeWithBackdoorValues(void) {
    auto path = WriteTempConfig(
        "/useSfBackdoor 0\n"
        "* 2 3\n"
        "- -1 -1\n");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &e) {
        threw = true;
        assert(std::string(e.what()).find("invalid") != std::string::npos);
    }
    remove(path.c_str());
    assert(threw);
}

// 10. Invalid rule: backdoor mode (useSfBackdoor=1) with Hz-range values (>=20)
void TestInvalidRuleBackdoorModeWithHzValues(void) {
    auto path = WriteTempConfig(
        "/useSfBackdoor 1\n"
        "* 60 120\n"
        "- -1 -1\n");

    bool threw = false;
    try {
        ParseConfigFile(path);
    } catch (const FmtException &e) {
        threw = true;
        assert(std::string(e.what()).find("invalid") != std::string::npos);
    }
    remove(path.c_str());
    assert(threw);
}

// 11. Per-app rules parsed correctly
void TestPerAppRules(void) {
    auto path = WriteTempConfig(
        "* 60 120\n"
        "- -1 -1\n"
        "com.game.a 30 60\n"
        "com.game.b 60 60\n"
        "com.game.c 90 120\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.rules.size() == 3);
    assert(cfg.rules["com.game.a"].idle == 30);
    assert(cfg.rules["com.game.a"].active == 60);
    assert(cfg.rules["com.game.b"].idle == 60);
    assert(cfg.rules["com.game.b"].active == 60);
    assert(cfg.rules["com.game.c"].idle == 90);
    assert(cfg.rules["com.game.c"].active == 120);
}

// 12. Tunables parsed correctly with clamping
void TestTunablesClamping(void) {
    auto path = WriteTempConfig(
        "/touchSlackMs 50\n"        // below MIN_TOUCH_SLACK_MS (100), should clamp to 100
        "/enableMinBrightness 300\n" // above MAX (255), should clamp to 255
        "* 60 120\n"
        "- -1 -1\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.touchSlackMs == 100);          // clamped up to MIN_TOUCH_SLACK_MS
    assert(cfg.enableMinBrightness == 255);   // clamped down to MAX
}

// 13. Valid backdoor config
void TestValidBackdoorConfig(void) {
    auto path = WriteTempConfig(
        "/useSfBackdoor 1\n"
        "* 2 5\n"
        "- -1 -1\n"
        "com.game 3 4\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.useSfBackdoor == true);
    assert(cfg.universial.idle == 2);
    assert(cfg.universial.active == 5);
    assert(cfg.rules["com.game"].idle == 3);
    assert(cfg.rules["com.game"].active == 4);
}

// 14. Offscreen rule with non-default values
void TestOffscreenWithNonDefaultValues(void) {
    auto path = WriteTempConfig(
        "* 60 120\n"
        "- 30 30\n");

    auto cfg = ParseConfigFile(path);
    remove(path.c_str());

    assert(cfg.hasOffscreen == true);
    assert(cfg.offscreen.idle == 30);
    assert(cfg.offscreen.active == 30);
}

// ---------- Main ----------

int main(void) {
    std::cout << "Running config parser tests..." << std::endl;

    RUN_TEST(TestValidFullConfig);
    RUN_TEST(TestMissingUniversalRule);
    RUN_TEST(TestMissingOffscreenRule);
    RUN_TEST(TestNonExistentFile);
    RUN_TEST(TestEmptyFile);
    RUN_TEST(TestCommentsOnlyFile);
    RUN_TEST(TestMalformedLinesSkipped);
    RUN_TEST(TestUnknownTunable);
    RUN_TEST(TestInvalidRuleHzModeWithBackdoorValues);
    RUN_TEST(TestInvalidRuleBackdoorModeWithHzValues);
    RUN_TEST(TestPerAppRules);
    RUN_TEST(TestTunablesClamping);
    RUN_TEST(TestValidBackdoorConfig);
    RUN_TEST(TestOffscreenWithNonDefaultValues);

    std::cout << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;

    return failed > 0 ? 1 : 0;
}
