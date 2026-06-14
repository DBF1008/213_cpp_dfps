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

#include <map>
#include <string>

struct FpsRule {
    int idle;
    int active;
};

struct ParsedConfig {
    bool useSfBackdoor = false;
    int64_t touchSlackMs = 4000;
    int enableMinBrightness = 8;

    std::map<std::string, FpsRule> rules;
    FpsRule offscreen = {-1, -1};
    FpsRule universial = {-1, -1};
    bool hasUniversial = false;
    bool hasOffscreen = false;
};

// Parse and validate the config file at configPath.
// Throws FmtException on any validation error (missing required rules,
// invalid rule values, file not found).  Pure function — no global side
// effects, no singleton instantiation.
ParsedConfig ParseConfigFile(const std::string &configPath);
