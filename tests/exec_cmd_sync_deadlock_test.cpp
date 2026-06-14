/*
 * Regression test for the ExecCmdSync pipe-buffer deadlock.
 *
 * Bug: when the child writes more data than the kernel pipe buffer can hold
 * (~64 KiB on Linux), a parent that calls waitpid() before draining the pipe
 * will deadlock -- the child blocks in write(), the parent blocks in
 * waitpid().
 *
 * This harness forks a child via ExecCmdSync() and asks it to emit several
 * hundred KiB of output, well above the pipe capacity. The test passes if the
 * call returns within a short timeout and the collected output matches the
 * expected size and content. A timeout means the deadlock has regressed.
 *
 * Build (host toolchain, no NDK required):
 *   c++ -std=c++17 -O0 -g -Isource -Itests \
 *       tests/exec_cmd_sync_deadlock_test.cpp source/utils/misc.cpp \
 *       -pthread -o exec_cmd_sync_deadlock_test
 *
 * Run:
 *   ./exec_cmd_sync_deadlock_test
 */

#include "utils/misc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// A single run of ExecCmdSync under alarm(). Returns the wall-clock duration
// in milliseconds, or -1 if the alarm fired (deadlock).
static long run_once(size_t targetBytes, std::string *out, int *exitCode) {
    // The child just emits `targetBytes` bytes of deterministic content and
    // exits 0. Using /usr/bin/head -c would work too but we want the exact
    // byte pattern to be verifiable and we don't want to depend on busybox vs
    // coreutils on the host.
    std::string awkProg = "BEGIN{ s=\"\"; while(length(s)<" + std::to_string(targetBytes) +
                          "){s=s\"ABCDEFGHIJ\"} printf \"%s\", substr(s,1," +
                          std::to_string(targetBytes) + ") }";

    const char *argv[] = {"/usr/bin/awk", awkProg.c_str(), nullptr};

    struct sigaction sa{}, oldSa{};
    sa.sa_handler = [](int) {};  // SIGALRM interrupts waitpid/read
    sa.sa_flags = 0;             // no SA_RESTART -- we want interruption
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, &oldSa);

    auto t0 = std::chrono::steady_clock::now();
    // 5 seconds is generous; a working implementation finishes in <100 ms for
    // this payload on any modern host.
    alarm(5);

    int rc = ExecCmdSync(out, argv);

    alarm(0);
    auto t1 = std::chrono::steady_clock::now();

    sigaction(SIGALRM, &oldSa, nullptr);

    *exitCode = rc;
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static int failures = 0;

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    } else {
        std::fprintf(stdout, "PASS: %s\n", msg);
    }
}

// ExecCmdSync's documented contract: on success, `content` holds the captured
// bytes NUL-terminated. In practice that means content->size() == N + 1 where
// the last byte is '\0' (this matches the callers in misc_android.cpp which
// pass content->data() to strstr/strchr and rely on NUL termination).
static bool outputHolds(std::string *content, size_t expectedBytes) {
    if (content->size() != expectedBytes + 1) return false;
    return content->back() == '\0';
}

int main() {
    // --- Case 1: tiny output (sanity). -----------------------------------
    {
        std::string out;
        int rc = -1;
        long ms = run_once(128, &out, &rc);
        check(rc == 0, "tiny output: exit status 0");
        check(outputHolds(&out, 128), "tiny output: length matches (NUL-terminated)");
        // Pattern check over the actual payload bytes (exclude trailing NUL).
        bool patternOk = true;
        for (size_t i = 0; i + 1 < out.size(); ++i) {
            if (out[i] != "ABCDEFGHIJ"[i % 10]) {
                patternOk = false;
                break;
            }
        }
        check(patternOk, "tiny output: content pattern matches");
        check(ms < 4000, "tiny output: completed quickly");
    }

    // --- Case 2: output >> pipe buffer (deadlock reproducer). ------------
    // 256 KiB is comfortably above the 64 KiB Linux pipe capacity and the
    // 16 KiB historical macOS default, so this exercises the bug on both.
    {
        const size_t N = 256 * 1024;
        std::string out;
        int rc = -1;
        long ms = run_once(N, &out, &rc);
        check(ms < 4000, "large output: did NOT deadlock (completed under timeout)");
        check(rc == 0, "large output: exit status 0");
        check(outputHolds(&out, N), "large output: full length captured (NUL-terminated)");

        // Spot-check head/tail pattern to catch partial reads / corruption.
        // Skip the trailing NUL at out[N].
        bool headOk = true, tailOk = true;
        for (size_t i = 0; i < 64 && i < N; ++i) {
            if (out[i] != "ABCDEFGHIJ"[i % 10]) {
                headOk = false;
                break;
            }
        }
        for (size_t i = (N > 64 ? N - 64 : 0); i < N; ++i) {
            if (out[i] != "ABCDEFGHIJ"[i % 10]) {
                tailOk = false;
                break;
            }
        }
        check(headOk, "large output: head pattern matches");
        check(tailOk, "large output: tail pattern matches");
    }

    // --- Case 3: no output collected (content == nullptr) must not hang. --
    {
        const char *argv[] = {"/bin/sh", "-c", "echo hello >/dev/null", nullptr};
        int rc = ExecCmdSync(nullptr, argv);
        check(rc == 0, "null content: exit status 0");
    }

    // --- Case 4: child exits non-zero, small output still collected. ------
    {
        const char *argv[] = {"/bin/sh", "-c", "echo oops; exit 7", nullptr};
        std::string out;
        int rc = ExecCmdSync(&out, argv);
        check(rc == 7, "non-zero exit: status propagated");
        check(out.find("oops") != std::string::npos, "non-zero exit: output captured");
    }

    if (failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d TEST(S) FAILED\n", failures);
    return 1;
}
