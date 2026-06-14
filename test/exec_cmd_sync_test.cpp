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

// Regression test for the ExecCmdSync() deadlock.
//
// Historically ExecCmdSync() called waitpid() to reap the child BEFORE it read the
// child's output pipe. A child that writes more than the pipe buffer can hold (~64 KiB
// on Linux) blocks in write() waiting for the reader to drain the pipe, while the parent
// blocks in waitpid() waiting for the child to exit -> both sides wait forever. On a real
// device this could hang top-app detection and brightness sampling whenever
// `dumpsys activity` produced a large dump.
//
// These tests drive the *real* ExecCmdSync() (linked from source/utils/misc.cpp) and feed
// it commands whose output is far larger than any pipe buffer. Every call runs under an
// alarm() watchdog, so the historical deadlock surfaces as a test FAILURE (watchdog fires)
// rather than an infinite hang. The fixed implementation drains the pipe before reaping,
// so the calls return promptly with the full output captured.
//
// The test only uses absolute command paths (/bin/cat, /bin/sh, /bin/echo) because
// ExecCmd() execv()s argv[0] without a PATH lookup. These paths exist on both Linux and
// macOS, so the test builds and runs on either host with a plain C++17 compiler.

#include "utils/misc.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "  [FAIL] %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond);                           \
        }                                                                                                              \
    } while (0)

// If ExecCmdSync() deadlocks, this fires and fails the whole run instead of hanging CI.
void OnWatchdog(int) {
    static const char msg[] =
        "\n[FAIL] watchdog timeout: ExecCmdSync() did not return -- likely deadlocked "
        "(child blocked writing a full pipe while the parent waited in waitpid)\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
    _exit(2);
}

// Deterministic, NUL-free, printable payload (digits + periodic newlines).
std::string MakePayload(size_t n) {
    std::string s;
    s.resize(n);
    for (size_t i = 0; i < n; ++i) {
        s[i] = (i % 64 == 63) ? '\n' : static_cast<char>('0' + (i % 10));
    }
    return s;
}

std::string WriteTempFile(const std::string &data) {
    char tmpl[] = "/tmp/dfps_exec_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        std::perror("mkstemp");
        _exit(3);
    }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = write(fd, data.data() + off, data.size() - off);
        if (w <= 0) {
            std::perror("write tempfile");
            _exit(3);
        }
        off += static_cast<size_t>(w);
    }
    close(fd);
    return std::string(tmpl);
}

// ExecCmdSync() returns the captured bytes followed by a trailing '\0' (so .size() is the
// byte count + 1), or an empty string when there was no output. Compare against that
// contract.
bool CapturedEquals(const std::string &out, const std::string &expected) {
    if (out.empty()) {
        return expected.empty();
    }
    if (out.back() != '\0') {
        return false; // the fixed implementation always NUL-terminates non-empty output
    }
    size_t capturedLen = out.size() - 1;
    if (capturedLen != expected.size()) {
        return false;
    }
    return std::memcmp(out.data(), expected.data(), capturedLen) == 0;
}

void Run(const char *name) {
    std::printf("[ RUN ] %s\n", name);
    std::fflush(stdout);
}

} // namespace

int main() {
    std::signal(SIGALRM, OnWatchdog);

    // ~1 MiB: comfortably larger than the pipe buffer on Linux (64 KiB) and macOS.
    const std::string payload = MakePayload(1u << 20);
    const std::string path = WriteTempFile(payload);

    // 1) Large stdout via /bin/cat: the exact shape of the historical deadlock.
    Run("large stdout is captured without deadlock");
    {
        std::string out;
        alarm(15);
        int rc = ExecCmdSync(&out, "/bin/cat", path.c_str());
        alarm(0);
        CHECK(rc == 0);
        CHECK(CapturedEquals(out, payload));
    }

    // 2) Large stderr: ExecCmd() dups the pipe onto both stdout and stderr, so a flood on
    //    stderr alone must also be drained.
    Run("large stderr is captured without deadlock");
    {
        std::string cmd = "cat '" + path + "' 1>&2";
        std::string out;
        alarm(15);
        int rc = ExecCmdSync(&out, "/bin/sh", "-c", cmd.c_str());
        alarm(0);
        CHECK(rc == 0);
        CHECK(CapturedEquals(out, payload));
    }

    // 3) Large stdout + stderr combined (~2 MiB total through one pipe).
    Run("combined stdout+stderr is captured without deadlock");
    {
        std::string cmd = "cat '" + path + "'; cat '" + path + "' 1>&2";
        std::string out;
        alarm(15);
        int rc = ExecCmdSync(&out, "/bin/sh", "-c", cmd.c_str());
        alarm(0);
        CHECK(rc == 0);
        CHECK(CapturedEquals(out, payload + payload));
    }

    // 4) Small output is captured verbatim (behavior preserved for the common case).
    Run("small output captured verbatim");
    {
        std::string out;
        alarm(10);
        int rc = ExecCmdSync(&out, "/bin/echo", "hello");
        alarm(0);
        CHECK(rc == 0);
        CHECK(CapturedEquals(out, "hello\n"));
    }

    // 5) No output -> empty string, even if the buffer held stale data.
    Run("empty output yields empty string");
    {
        std::string out = "stale-contents-should-be-cleared";
        alarm(10);
        int rc = ExecCmdSync(&out, "/bin/sh", "-c", "exit 0");
        alarm(0);
        CHECK(rc == 0);
        CHECK(out.empty());
    }

    // 6) Exit status is propagated, including on the content==nullptr path.
    Run("exit status is propagated (no content pipe)");
    {
        alarm(10);
        int rc = ExecCmdSync(nullptr, "/bin/sh", "-c", "exit 7");
        alarm(0);
        CHECK(rc == 7);
    }

    unlink(path.c_str());

    if (g_failures == 0) {
        std::printf("\n[ PASS ] all %d checks passed\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "\n[ FAIL ] %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
