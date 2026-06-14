// Stub for host-side (macOS) regression build. On Linux, <sys/prctl.h> is
// provided by glibc; on Android it's in bionic. ExecCmdSync doesn't depend
// on it, so this shim only declares the symbols misc.cpp references so the
// deadlock test can link.
#pragma once

#ifndef PR_SET_NAME
#define PR_SET_NAME 15
#endif

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int prctl(int /*option*/, const char *name) {
#if defined(__APPLE__)
    // pthread_setname_np on macOS takes the thread name for the current
    // thread; this is the closest equivalent and is enough to link.
    return pthread_setname_np(name ? name : "dfps");
#else
    (void)name;
    return 0;
#endif
}

// pipe2() is a Linux/BSD extension. On macOS we emulate it via pipe() +
// fcntl(FD_CLOEXEC); only the test harness hits this path.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <unistd.h>
static inline int pipe2(int pipefd[2], int flags) {
    int rc = pipe(pipefd);
    if (rc != 0) return rc;
    if (flags & O_CLOEXEC) {
        fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
