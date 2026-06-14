/*
 * Platform and utility stubs for testing DynamicFps on host.
 * Provides minimal implementations of ModuleBase, DelayedWorker, HeavyWorker,
 * CoBridge, and Android-specific functions.
 */

/* macOS doesn't have timer_t in <time.h>; provide a stub typedef */
#ifdef __APPLE__
#include <sys/types.h>
#ifndef _TIMER_T_DEFINED_
typedef unsigned long timer_t;
#define _TIMER_T_DEFINED_
#endif
#endif

#include "platform/module_base.h"
#include "platform/delayed_worker.h"
#include "platform/heavy_worker.h"
#include "platform/cobridge.h"
#include "utils/misc.h"
#include "utils/misc_android.h"
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <string>
#include <unistd.h>

/* --- Test hooks: allow tests to control backend behavior --- */

namespace test_hooks {

// Backend function hooks — tests set these to control behavior
std::function<bool(const std::string &, bool)> peakRefreshRateHook;
std::function<bool(const std::string &, bool)> sfBackdoorHook;
std::function<bool(void)> probePeakHook;
std::function<bool(void)> probeSfHook;

// Notification tracking
std::string lastNotifyContent;
int notifyWriteCount = 0;

// ExecCmdSync hook for settings get/put
std::function<int(std::string *, const char **)> execCmdSyncHook;

void reset() {
    peakRefreshRateHook = nullptr;
    sfBackdoorHook = nullptr;
    probePeakHook = nullptr;
    probeSfHook = nullptr;
    lastNotifyContent.clear();
    notifyWriteCount = 0;
    execCmdSyncHook = nullptr;
}

} // namespace test_hooks

/* --- DelayedWorker / HeavyWorker stubs --- */

DelayedWorker::DelayedWorker() : isThreadInited_(false), timerId_(0) {}
DelayedWorker::~DelayedWorker() {}
DelayedWorker::Handle DelayedWorker::Create(const std::string &) { return 0; }
void DelayedWorker::SetWork(Handle, const Work &, int64_t) {}
void DelayedWorker::_OnTimer() {}
void DelayedWorker::ReloadTimer() {}
int DelayedWorker::FindEarliestWorkIdx() { return -1; }
int DelayedWorker::FindEarliestReadyWorkIdx() { return -1; }

HeavyWorker::HeavyWorker() : pending_(false) {}
HeavyWorker::Handle HeavyWorker::Create(const std::string &) { return 0; }
void HeavyWorker::SetWork(Handle, const Work &work) {
    // Execute synchronously in tests
    if (work) {
        work();
    }
}

/* --- CoBridge stub --- */

CoBridge::CoBridge() {}
void CoBridge::Publish(const std::string &, const void *) const {}
void CoBridge::Subscribe(const std::string &, const SubscribeCallback &) {}
bool CoBridge::HasSubscriber(const std::string &) const { return false; }

/* --- ModuleBase (using real implementation) --- */

/* --- misc stubs --- */

int64_t GetNowTs(void) { return 0; }
void Sleep(int64_t) {}
bool IsSpace(const char c) { return c == ' ' || c == '\t' || c == '\n'; }
bool IsDigit(const char c) { return c >= '0' && c <= '9'; }

bool ParseInt(const char *s, int32_t *val) {
    *val = 0;
    int sign = 1;
    for (const char *c = s; *c; ++c) {
        if (*c == '-') sign = -1;
        if (IsDigit(*c)) { s = c; break; }
    }
    int len = 0;
    for (const char *c = s; *c && IsDigit(*c); ++c) ++len;
    if (len == 0) return false;
    for (int i = 0; i < len; ++i) {
        *val = *val * 10 + (s[i] - '0');
    }
    *val *= sign;
    return true;
}

std::string_view RefKey(const std::string &fullname) {
    std::string_view view = fullname;
    auto pos = view.find_first_of('.');
    return (pos < view.length()) ? view.substr(pos + 1) : std::string_view();
}

static char stub_argv[] = "dfps_test";
void InitArgv(int, char **) {}
void SetSelfName(const std::string_view &) {}
void SetSelfThreadName(const std::string_view &) {}

int ReadFile(const std::string_view &, std::string *, size_t) { return -1; }

int WriteSysfsFile(int fd, const std::string_view &s) {
    if (fd > 0) {
        test_hooks::lastNotifyContent = std::string(s);
        test_hooks::notifyWriteCount++;
        return write(fd, s.data(), s.length());
    }
    return -1;
}

int WriteSysfsFile(int fd, int s) {
    if (fd > 0) return write(fd, &s, sizeof(s));
    return -1;
}

int WriteSysfsFile(const std::string_view &path, const std::string_view &s) {
    auto fd = GetFdForWrite(path);
    if (fd > 0) {
        auto ret = WriteSysfsFile(fd, s);
        close(fd);
        return ret;
    }
    return -1;
}

int WriteSysfsFile(const std::string_view &path, int s) {
    auto fd = GetFdForWrite(path);
    if (fd > 0) {
        auto ret = WriteSysfsFile(fd, s);
        close(fd);
        return ret;
    }
    return -1;
}

int GetFdForWrite(const std::string_view &path) {
    return open(path.data(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_CREAT | O_TRUNC, 0666);
}

int GetFdForWriteExcluded(const std::string_view &path) {
    return GetFdForWrite(path);
}

int ExecCmd(int *, const char **) { return -1; }

int ExecCmdSync(std::string *content, const char **argv) {
    if (test_hooks::execCmdSyncHook) {
        return test_hooks::execCmdSyncHook(content, argv);
    }
    return -1;
}

/* --- misc_android stubs --- */

int GetOSVersion(void) { return 12; }
int GetScreenBrightness(void) { return 128; }

bool SysPeakRefreshRate(const std::string &hz, bool force) {
    if (test_hooks::peakRefreshRateHook) {
        return test_hooks::peakRefreshRateHook(hz, force);
    }
    return true;
}

bool SysSurfaceflingerBackdoor(const std::string &idx, bool force) {
    if (test_hooks::sfBackdoorHook) {
        return test_hooks::sfBackdoorHook(idx, force);
    }
    return true;
}

bool ProbePeakRefreshRateBackend(void) {
    if (test_hooks::probePeakHook) {
        return test_hooks::probePeakHook();
    }
    return false;
}

bool ProbeSurfaceflingerBackdoor(void) {
    if (test_hooks::probeSfHook) {
        return test_hooks::probeSfHook();
    }
    return false;
}

/* --- SchedCtrl stub --- */

extern "C" {
void SchedCtrlSetStaticPrio(int, int, bool) {}
}
