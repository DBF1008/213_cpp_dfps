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

#pragma once

#include <cstdlib>
#include <unordered_set>
#include <vector>

// Heuristic that decides, from two consecutive snapshots of the top-app cpuset
// task list (/dev/cpuset/top-app/tasks, i.e. the TIDs of the foreground apps),
// whether the foreground app may have changed and the package name therefore
// needs to be re-queried with the (relatively expensive) dumpsys call.
//
// Two independent, cheap signals are OR'd together:
//
//   1. Task-count delta -- |#cur - #prev| > nrDiffMin.
//      The original heuristic. Catches large changes in foreground scale, e.g.
//      a cold launch ballooning the thread count or an app being killed.
//
//   2. Task-set churn -- a large fraction of the *previous* foreground's tasks
//      have left the cpuset: gone / #prev >= goneRatio.
//      This is what the count delta alone misses: when two foreground apps have
//      a similar number of threads, #cur - #prev is ~0, yet the actual TIDs are
//      almost entirely different. Comparing task *identities* instead of the
//      task *count* makes app<->app switches, launcher switches and
//      gesture-back-to-another-app detectable regardless of how close the two
//      foregrounds' thread counts happen to be.
//
// The function is pure / side-effect free so the decision can be exercised by a
// host unit test (see test/topapp_switch_detector_test.cpp).
//
//   prev      previous accepted snapshot (empty before the first snapshot)
//   cur       current snapshot
//   nrDiffMin count-delta threshold for signal 1
//   goneRatio fraction in (0, 1]; how much of prev must vanish for signal 2
//
// The thresholds default to values tuned for /dev/cpuset/top-app/tasks (TID
// granularity) and are the single source of truth shared by the production
// module and the regression test:
//   * within one app the long-lived threads persist between snapshots, so the
//     gone fraction stays near 0 -> no spurious re-query on thread-pool jitter;
//   * a real switch evicts (almost) all of the old foreground's threads, so the
//     gone fraction jumps to ~1 -> the switch is caught even at equal scale.
inline bool TopappTasksChanged(const std::vector<int> &prev, const std::vector<int> &cur, size_t nrDiffMin = 10,
                               double goneRatio = 0.5) {
    // First snapshot, or the foreground was emptied and repopulated: there is
    // no meaningful baseline to compare against, so (re)identify whenever the
    // current foreground is non-empty.
    if (prev.empty()) {
        return !cur.empty();
    }

    // Signal 1: the foreground thread count changed by a lot.
    auto delta = std::abs(static_cast<long>(cur.size()) - static_cast<long>(prev.size()));
    if (delta > static_cast<long>(nrDiffMin)) {
        return true;
    }

    // Signal 2: a (configurable) majority of the previous foreground's tasks
    // are gone from the cpuset -- the old top app is no longer on top.
    std::unordered_set<int> curSet(cur.begin(), cur.end());
    size_t gone = 0;
    for (int tid : prev) {
        if (curSet.count(tid) == 0) {
            ++gone;
        }
    }
    double goneFrac = static_cast<double>(gone) / static_cast<double>(prev.size());
    return goneFrac >= goneRatio;
}
