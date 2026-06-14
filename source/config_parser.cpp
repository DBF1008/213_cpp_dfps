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

#include "config_parser.h"
#include "utils/fmt_exception.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdio>
#include <string>

constexpr int MIN_TOUCH_SLACK_MS = 100;
constexpr int MAX_ENABLE_MIN_BRIGHTNESS = 255;
constexpr char UNIVERSIAL_PKG_NAME[] = "*";
constexpr char OFFSCREEN_PKG_NAME[] = "-";

static std::string Trim(const std::string &str) {
    if (str.empty()) {
        return str;
    }
    auto firstScan = str.find_first_not_of(" \n\r");
    auto first = (firstScan == std::string::npos) ? str.length() : firstScan;
    auto last = str.find_last_not_of(" \n\r");
    return str.substr(first, last - first + 1);
}

static void SetTunable(ParsedConfig &cfg, const std::string &tunable, const std::string &value) {
    try {
        if (tunable == "useSfBackdoor") {
            cfg.useSfBackdoor = (std::stoi(value) > 0);
        } else if (tunable == "touchSlackMs") {
            cfg.touchSlackMs = std::max((int)MIN_TOUCH_SLACK_MS, std::stoi(value));
        } else if (tunable == "enableMinBrightness") {
            cfg.enableMinBrightness = std::min(MAX_ENABLE_MIN_BRIGHTNESS, std::stoi(value));
        } else {
            SPDLOG_WARN("Unknown tunable '{}' in the config file", tunable);
        }
    } catch (const std::exception &e) {
        throw FmtException("Invalid value for tunable '{}': {}", tunable, e.what());
    }
}

static void AddRule(ParsedConfig &cfg, const std::string &pkgName, FpsRule rule) {
    if (pkgName == UNIVERSIAL_PKG_NAME) {
        cfg.hasUniversial = true;
        cfg.universial = rule;
    } else if (pkgName == OFFSCREEN_PKG_NAME) {
        cfg.hasOffscreen = true;
        cfg.offscreen = rule;
    } else {
        SPDLOG_DEBUG("Load '{}', dfps={}/{}", pkgName, rule.idle, rule.active);
        cfg.rules.emplace(pkgName, rule);
    }
}

static void ParseLine(ParsedConfig &cfg, const std::string &line) {
    char name[256];
    char value[256];
    name[0] = '\0';
    value[0] = '\0';

    if (line.empty() || line[0] == '#') {
        return;
    } else if (line[0] == '/') {
        // /touchSlackMs 4000
        if (sscanf(line.c_str(), "/%s %s", name, value) == 2) {
            SPDLOG_DEBUG("Set '{}'={}", name, value);
            SetTunable(cfg, name, value);
        } else {
            SPDLOG_WARN("Skipped broken line '{}'", line);
        }
    } else {
        // com.example.app 60 120
        // com.example.app 2 0
        FpsRule rule;
        if (sscanf(line.c_str(), "%s %d %d", name, &rule.idle, &rule.active) == 3) {
            AddRule(cfg, name, rule);
        } else {
            SPDLOG_WARN("Skipped broken line '{}'", line);
        }
    }
}

static std::string FindInvalidRule(const ParsedConfig &cfg) {
    auto isDefaultRule = [](const FpsRule &rule) { return rule.idle == -1 && rule.active == -1; };
    auto isSfBackdoorRule = [](const FpsRule &rule) { return rule.idle < 20 && rule.active < 20; };
    auto isInvalid = [&](const FpsRule &rule) {
        return isDefaultRule(rule) == false && cfg.useSfBackdoor != isSfBackdoorRule(rule);
    };

    if (isInvalid(cfg.offscreen)) {
        return "offscreen";
    }
    if (isInvalid(cfg.universial)) {
        return "default";
    }
    for (const auto &[name, rule] : cfg.rules) {
        if (isInvalid(rule)) {
            return name;
        }
    }

    return {};
}

ParsedConfig ParseConfigFile(const std::string &configPath) {
    FILE *fp = fopen(configPath.c_str(), "re");
    if (fp == NULL) {
        throw FmtException("Cannot open config '{}'", configPath);
    }

    ParsedConfig cfg;

    char buf[256];
    while (feof(fp) == false) {
        buf[0] = '\0';
        fgets(buf, sizeof(buf), fp);
        auto line = Trim(buf);
        ParseLine(cfg, line);
    }
    fclose(fp);

    if (cfg.hasOffscreen == false) {
        throw FmtException("Offscreen rule not specified in the config file");
    }
    if (cfg.hasUniversial == false) {
        throw FmtException("Default rule not specified in the config file");
    }
    auto invalidRuleName = FindInvalidRule(cfg);
    if (invalidRuleName.empty() == false) {
        throw FmtException("Rule of '{}' is invalid", invalidRuleName);
    }

    return cfg;
}
