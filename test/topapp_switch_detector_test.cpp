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

// Host regression test for the top-app switch detection heuristic.
//
// It pins the behaviour that the production module relies on, in particular the
// case the old count-only heuristic missed: switching between two foreground
// apps whose thread counts are similar (disjoint TID sets, ~zero count delta).
//
// Build & run:  sh test/run_tests.sh      (or: c++ -std=c++17 this_file && ./a.out)

#include "../source/modules/topapp_switch_detector.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failed = 0;
int g_total = 0;

void Check(bool cond, const char *expr, const char *desc, int line) {
    ++g_total;
    if (!cond) {
        ++g_failed;
        std::fprintf(stderr, "  FAIL [line %d] %s\n        (%s)\n", line, desc, expr);
    }
}

#define CHECK(cond, desc) Check((cond), #cond, (desc), __LINE__)

// Decide using the same defaults the production module compiles in.
bool Changed(const std::vector<int> &prev, const std::vector<int> &cur) {
    return TopappTasksChanged(prev, cur); // default thresholds: nrDiffMin=10, goneRatio=0.5
}

// [base, base + n) as a TID list. Distinct bases model distinct apps/threads.
std::vector<int> Range(int base, int n) {
    std::vector<int> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        v.push_back(base + i);
    }
    return v;
}

// `shared` of prev's leading TIDs kept, plus `fresh` brand-new TIDs appended;
// total size stays `shared + fresh`. Used to dial the "gone" fraction exactly.
std::vector<int> Mix(const std::vector<int> &prev, int shared, int fresh) {
    std::vector<int> v(prev.begin(), prev.begin() + shared);
    auto add = Range(900000, fresh); // 900000+ never overlaps the app ranges below
    v.insert(v.end(), add.begin(), add.end());
    return v;
}

} // namespace

int main() {
    // ---- baseline / empty handling ----------------------------------------
    CHECK(Changed({}, {}) == false, "no foreground yet -> no query");
    CHECK(Changed({}, Range(1, 50)) == true, "first snapshot -> identify once");
    CHECK(Changed(Range(1, 50), {}) == true, "foreground emptied -> re-evaluate");

    // ---- same app: must NOT re-query (keeps dumpsys overhead bounded) ------
    CHECK(Changed(Range(1, 100), Range(1, 100)) == false, "identical snapshot -> stable");
    {
        // a few worker threads recycled, identical count: the exact shape that
        // makes thread-pool jitter look like noise rather than a switch.
        auto prev = Range(1, 100);
        auto cur = prev;
        cur[10] = 500001;
        cur[20] = 500002;
        cur[30] = 500003; // 3/100 gone
        CHECK(Changed(prev, cur) == false, "small thread jitter -> stable");
    }

    // ---- THE BUG: same-scale app switch the count delta misses -------------
    {
        auto appA = Range(1, 100);      // com.foo : 100 threads
        auto appB = Range(200000, 100); // com.bar : 100 threads, disjoint TIDs

        // Model the original count-only rule (fire only when the thread count
        // moves by more than the threshold) to make the regression explicit:
        // it is blind to an equal-scale switch, which is exactly the漏判 we fix.
        auto oldCountOnly = [](const std::vector<int> &p, const std::vector<int> &c) {
            return std::abs(static_cast<long>(c.size()) - static_cast<long>(p.size())) > 10;
        };
        CHECK(oldCountOnly(appA, appB) == false, "old count-only rule misses equal-scale switch");
        CHECK(Changed(appA, appB) == true, "equal-scale app switch -> re-query (regression)");
    }
    {
        // launcher / gesture-back to a different app: majority of old TIDs gone.
        auto app = Range(1, 100);
        auto launcher = Mix(app, 40, 60); // 60/100 gone
        CHECK(Changed(app, launcher) == true, "switch with minority overlap -> re-query");
    }

    // ---- count-delta signal (preserves original behaviour) ----------------
    CHECK(Changed(Range(1, 20), Range(1, 40)) == true, "cold launch balloon -> re-query");
    CHECK(Changed(Range(1, 40), Range(1, 20)) == true, "large shrink -> re-query");

    // ---- tuning boundaries ------------------------------------------------
    // count delta is strictly-greater-than: 10 stays, 11 fires.
    {
        auto prev = Range(1, 100);
        auto plus10 = prev;
        auto a = Range(800000, 10);
        plus10.insert(plus10.end(), a.begin(), a.end()); // delta 10, gone 0
        CHECK(Changed(prev, plus10) == false, "count delta == nrDiffMin -> stable");

        auto plus11 = prev;
        auto b = Range(800000, 11);
        plus11.insert(plus11.end(), b.begin(), b.end()); // delta 11
        CHECK(Changed(prev, plus11) == true, "count delta > nrDiffMin -> re-query");
    }
    // gone fraction is >=: exactly half fires, just under does not. Size is held
    // at 100 in both so only the churn signal is under test.
    {
        auto prev = Range(1, 100);
        CHECK(Changed(prev, Mix(prev, 50, 50)) == true, "gone fraction == goneRatio -> re-query");
        CHECK(Changed(prev, Mix(prev, 51, 49)) == false, "gone fraction < goneRatio -> stable");
    }

    if (g_failed == 0) {
        std::printf("ok: all %d topapp-switch-detector checks passed\n", g_total);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d/%d checks\n", g_failed, g_total);
    return 1;
}
