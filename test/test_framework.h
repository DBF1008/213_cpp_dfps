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

// Tiny dependency-free test harness. Tests self-register with the TEST() macro
// and run from a single main() (test_main.cpp). RunAll() returns non-zero when
// any check fails so it can gate a build.

#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace t {

inline int &Failures() {
    static int f = 0;
    return f;
}

inline int &Checks() {
    static int c = 0;
    return c;
}

struct Case {
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &Cases() {
    static std::vector<Case> v;
    return v;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) { Cases().push_back({name, std::move(fn)}); }
};

inline int RunAll() {
    int failedCases = 0;
    for (auto &c : Cases()) {
        int before = Failures();
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (Failures() > before) {
            std::printf("[ FAIL ] %s\n", c.name);
            ++failedCases;
        } else {
            std::printf("[  OK  ] %s\n", c.name);
        }
    }
    std::printf("\n%d checks, %d failure(s) across %zu case(s); %d case(s) failed\n", Checks(), Failures(),
                Cases().size(), failedCases);
    return failedCases == 0 ? 0 : 1;
}

} // namespace t

#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static ::t::Registrar registrar_##name(#name, name);                                                               \
    static void name()

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ::t::Checks()++;                                                                                               \
        if (!(cond)) {                                                                                                 \
            ::t::Failures()++;                                                                                         \
            std::printf("  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                   \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        ::t::Checks()++;                                                                                               \
        auto va_ = (a);                                                                                                \
        auto vb_ = (b);                                                                                                \
        if (!(va_ == vb_)) {                                                                                           \
            ::t::Failures()++;                                                                                         \
            std::ostringstream os_;                                                                                    \
            os_ << "got [" << va_ << "] expected [" << vb_ << "]";                                                     \
            std::printf("  CHECK_EQ failed: %s == %s -- %s (%s:%d)\n", #a, #b, os_.str().c_str(), __FILE__,           \
                        __LINE__);                                                                                     \
        }                                                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                          \
    do {                                                                                                               \
        ::t::Checks()++;                                                                                               \
        double va_ = (a);                                                                                              \
        double vb_ = (b);                                                                                              \
        if (std::fabs(va_ - vb_) > (tol)) {                                                                            \
            ::t::Failures()++;                                                                                         \
            std::printf("  CHECK_NEAR failed: %s ~= %s -- got %f expected %f (%s:%d)\n", #a, #b, va_, vb_,            \
                        __FILE__, __LINE__);                                                                           \
        }                                                                                                              \
    } while (0)
