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

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Parses and validates the dfps config file. This unit deliberately depends only on
// the C++ standard library (no platform/module_base, no spdlog, no fmt) so that the
// config can be validated cheaply by the daemon before it swaps the running instance,
// and so the validation logic can be unit-tested on the host.

struct FpsRule {
    int idle;
    int active;
};

class ConfigParser {
public:
    ConfigParser();

    // Parses and validates the config at `path`. Throws std::runtime_error on any fatal
    // error: the file cannot be opened, the offscreen ('-') or default ('*') rule is
    // missing, or a rule's idle/active values are inconsistent with useSfBackdoor.
    // Non-fatal issues (unparsable lines, unknown tunables) are appended to `warnings`.
    void Load(const std::string &path);

    // Convenience: parse-and-validate without keeping the result. Throws on invalid config.
    static void Validate(const std::string &path);

    bool useSfBackdoor;
    int64_t touchSlackMs;
    int enableMinBrightness;
    std::map<std::string, FpsRule> rules;
    FpsRule offscreen;
    FpsRule universial;
    bool hasUniversial;
    bool hasOffscreen;
    std::vector<std::string> warnings;

private:
    void ParseLine(const std::string &line);
    void AddRule(const std::string &pkgName, FpsRule rule);
    void SetTunable(const std::string &tunable, const std::string &value);
    std::string FindInvalidRule(void);
};
