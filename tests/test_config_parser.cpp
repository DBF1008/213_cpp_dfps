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

// Regression test for the dfps config validator.
//
// Bug being guarded: on a config hot-reload the daemon used to kill the running instance
// and only then start a replacement that validated the config in its constructor. A bad
// config (missing default '*' / offscreen '-' rule, an inconsistent rule, or a parse error)
// made the replacement exit immediately, so the whole dynamic-refresh-rate service went down.
//
// The fix moves validation into ConfigParser and makes the daemon validate the new config
// BEFORE touching the running instance, keeping the old instance alive on a bad config. These
// tests pin down exactly which configs ConfigParser::Validate accepts vs rejects (the predicate
// the daemon now branches on), and that pathological input neither crashes nor wrongly rejects.
//
// Host-only: ConfigParser depends solely on the C++ standard library, so this builds and runs
// with a plain C++17 compiler (no Android NDK, no spdlog, no device). See tests/run.sh.

#include "modules/config_parser.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

static void Pass(const std::string &name) {
    ++g_pass;
    std::cout << "  [PASS] " << name << "\n";
}

static void Fail(const std::string &name, const std::string &detail) {
    ++g_fail;
    std::cout << "  [FAIL] " << name << " — " << detail << "\n";
}

// Writes `content` to a fresh temp file and returns its path. Caller unlinks it.
static std::string WriteTempConfig(const std::string &content) {
    char tmpl[] = "/tmp/dfps_cfg_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        std::cerr << "mkstemp failed\n";
        std::exit(2);
    }
    if (!content.empty()) {
        ssize_t n = write(fd, content.data(), content.size());
        (void)n;
    }
    close(fd);
    return std::string(tmpl);
}

static void ExpectAccept(const std::string &name, const std::string &content) {
    auto path = WriteTempConfig(content);
    try {
        ConfigParser::Validate(path);
        Pass(name);
    } catch (const std::exception &e) {
        Fail(name, std::string("expected accept, but rejected: ") + e.what());
    }
    unlink(path.c_str());
}

static void ExpectReject(const std::string &name, const std::string &content, const std::string &wantSubstr) {
    auto path = WriteTempConfig(content);
    try {
        ConfigParser::Validate(path);
        Fail(name, "expected reject, but it was accepted");
    } catch (const std::exception &e) {
        std::string what = e.what();
        if (wantSubstr.empty() || what.find(wantSubstr) != std::string::npos) {
            Pass(name + " (rejected: " + what + ")");
        } else {
            Fail(name, "rejected but message '" + what + "' lacks '" + wantSubstr + "'");
        }
    }
    unlink(path.c_str());
}

static void Expect(const std::string &name, bool cond, const std::string &detail) {
    if (cond) {
        Pass(name);
    } else {
        Fail(name, detail);
    }
}

int main(void) {
    std::cout << "== ConfigParser regression tests ==\n";

    // --- Accepted configs: the daemon proceeds with the reload ---
    ExpectAccept("good config (PEAK_REFRESH_RATE mode)",
                 "/touchSlackMs 4000\n/enableMinBrightness 8\n/useSfBackdoor 0\n"
                 "com.example.app 90 120\n- -1 -1\n* 60 120\n");

    ExpectAccept("good config (surfaceflinger backdoor mode)",
                 "/useSfBackdoor 1\n- 1 1\n* 2 0\n");

    ExpectAccept("comments, blank lines and a junk line are tolerated",
                 "# header comment\n\ncom.example.app 60 60\n- -1 -1\n* 60 120\n");

    // --- Rejected configs: the daemon rolls back and keeps the running instance ---
    ExpectReject("missing default '*' rule", "- 60 60\n", "Default rule");
    ExpectReject("missing offscreen '-' rule", "* 60 120\n", "Offscreen rule");
    ExpectReject("inconsistent rule: sub-20hz values without backdoor",
                 "/useSfBackdoor 0\n- 60 60\n* 2 0\n", "invalid");
    ExpectReject("inconsistent rule: full-hz values with backdoor enabled",
                 "/useSfBackdoor 1\n- 60 60\n* 2 0\n", "invalid");
    ExpectReject("non-numeric tunable value aborts the parse",
                 "/touchSlackMs notanumber\n- 60 60\n* 60 120\n", "");

    // Nonexistent file: a separate path because it must NOT exist on disk.
    {
        const char *bogus = "/tmp/dfps_no_such_dir_zzz/does_not_exist.txt";
        try {
            ConfigParser::Validate(bogus);
            Fail("nonexistent config file", "expected reject, but it was accepted");
        } catch (const std::exception &e) {
            std::string what = e.what();
            if (what.find("Cannot open config") != std::string::npos) {
                Pass(std::string("nonexistent config file (rejected: ") + what + ")");
            } else {
                Fail("nonexistent config file", std::string("wrong message: ") + what);
            }
        }
    }

    // --- Hardening: pathological input must neither overflow (run under ASAN) nor wrongly reject.
    // A token far longer than the 256-byte parse buffers is a broken line -> skipped, not fatal.
    {
        std::string longTok(5000, 'a');
        ExpectAccept("over-long bare token line is skipped, not fatal",
                     "- 60 60\n* 60 120\n" + longTok + "\n");
        ExpectAccept("over-long tunable token line is skipped, not fatal",
                     "- 60 60\n* 60 120\n/" + longTok + " 1\n");
    }

    // --- White-box: lock the parsed values so a future refactor can't silently drift them. ---
    {
        auto path = WriteTempConfig("- 60 60\n* 60 120\n"); // no tunables -> runtime defaults
        ConfigParser p;
        p.Load(path);
        Expect("default touchSlackMs is 4000 when unset", p.touchSlackMs == 4000,
               "got " + std::to_string(p.touchSlackMs));
        Expect("default useSfBackdoor is false when unset", p.useSfBackdoor == false, "got true");
        Expect("default enableMinBrightness is 8 when unset", p.enableMinBrightness == 8,
               "got " + std::to_string(p.enableMinBrightness));
        unlink(path.c_str());
    }
    {
        auto path = WriteTempConfig("/touchSlackMs 5\n/enableMinBrightness 999\n- 60 60\n* 60 120\n");
        ConfigParser p;
        p.Load(path);
        Expect("touchSlackMs is clamped up to 100", p.touchSlackMs == 100,
               "got " + std::to_string(p.touchSlackMs));
        Expect("enableMinBrightness is clamped down to 255", p.enableMinBrightness == 255,
               "got " + std::to_string(p.enableMinBrightness));
        unlink(path.c_str());
    }
    {
        auto path = WriteTempConfig("totally broken line\n- 60 60\n* 60 120\n");
        ConfigParser p;
        p.Load(path);
        Expect("a broken line is recorded as a warning", !p.warnings.empty(),
               "expected at least one warning");
        unlink(path.c_str());
    }

    std::cout << "\n== " << g_pass << " passed, " << g_fail << " failed ==\n";
    return g_fail == 0 ? 0 : 1;
}
