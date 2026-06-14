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

// Authoritative screen power state, as reported by the system (e.g. `dumpsys power`).
// kUnknown means the system could not be queried or the output was unparseable; callers
// must treat it as "no information" and fall back to other signals rather than guessing.
//
// This is a dependency-free leaf header shared by the Android system helpers
// (utils/misc_android.h) and the pure offscreen decision policy
// (modules/offscreen_policy.h) so neither has to depend on the other.
enum class ScreenState {
    kUnknown,
    kOff,
    kOn,
};
