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

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {

constexpr int MIN_TOUCH_SLACK_MS = 100;
constexpr int MAX_ENABLE_MIN_BRIGHTNESS = 255;
constexpr char UNIVERSIAL_PKG_NAME[] = "*";
constexpr char OFFSCREEN_PKG_NAME[] = "-";

std::string Trim(const std::string &str) {
    if (str.empty()) {
        return str;
    }
    auto firstScan = str.find_first_not_of(" \n\r");
    auto first = (firstScan == std::string::npos) ? str.length() : firstScan;
    auto last = str.find_last_not_of(" \n\r");
    return str.substr(first, last - first + 1);
}

} // namespace

ConfigParser::ConfigParser()
    : useSfBackdoor(false),
      touchSlackMs(4000),
      enableMinBrightness(8),
      offscreen{0, 0},
      universial{0, 0},
      hasUniversial(false),
      hasOffscreen(false) {}

void ConfigParser::Validate(const std::string &path) {
    ConfigParser parser;
    parser.Load(path);
}

void ConfigParser::Load(const std::string &path) {
    FILE *fp = fopen(path.c_str(), "re");
    if (fp == NULL) {
        throw std::runtime_error("Cannot open config '" + path + "'");
    }

    char buf[256];
    while (feof(fp) == false) {
        buf[0] = '\0';
        (void)fgets(buf, sizeof(buf), fp);
        auto line = Trim(buf);
        ParseLine(line);
    }

    if (hasOffscreen == false) {
        fclose(fp);
        throw std::runtime_error("Offscreen rule not specified in the config file");
    }
    if (hasUniversial == false) {
        fclose(fp);
        throw std::runtime_error("Default rule not specified in the config file");
    }
    auto invalidRuleName = FindInvalidRule();
    if (invalidRuleName.empty() == false) {
        fclose(fp);
        throw std::runtime_error("Rule of '" + invalidRuleName + "' is invalid");
    }

    fclose(fp);
}

void ConfigParser::ParseLine(const std::string &line) {
    auto isComment = [](const std::string &line) { return line[0] == '#'; };
    auto isTunable = [](const std::string &line) { return line[0] == '/'; };

    char name[256];
    char value[256];
    name[0] = '\0';
    value[0] = '\0';

    if (line.empty() || isComment(line)) {
        return;
    } else if (isTunable(line)) {
        // /touchSlackMs 4000
        if (sscanf(line.c_str(), "/%255s %255s", name, value) == 2) {
            SetTunable(name, value);
        } else {
            warnings.emplace_back("Skipped broken line '" + line + "'");
        }
    } else {
        // com.example.app 60 120
        // com.example.app 2 0
        FpsRule rule;
        if (sscanf(line.c_str(), "%255s %d %d", name, &rule.idle, &rule.active) == 3) {
            AddRule(name, rule);
        } else {
            warnings.emplace_back("Skipped broken line '" + line + "'");
        }
    }
}

void ConfigParser::AddRule(const std::string &pkgName, FpsRule rule) {
    if (pkgName == UNIVERSIAL_PKG_NAME) {
        hasUniversial = true;
        universial = rule;
    } else if (pkgName == OFFSCREEN_PKG_NAME) {
        hasOffscreen = true;
        offscreen = rule;
    } else {
        rules.emplace(pkgName, rule);
    }
}

void ConfigParser::SetTunable(const std::string &tunable, const std::string &value) {
    if (tunable == "useSfBackdoor") {
        useSfBackdoor = (std::stoi(value) > 0) ? true : false;
    } else if (tunable == "touchSlackMs") {
        touchSlackMs = std::max(MIN_TOUCH_SLACK_MS, std::stoi(value));
    } else if (tunable == "enableMinBrightness") {
        enableMinBrightness = std::min(MAX_ENABLE_MIN_BRIGHTNESS, std::stoi(value));
    } else {
        warnings.emplace_back("Unknown tunable '" + tunable + "' in the config file");
    }
}

std::string ConfigParser::FindInvalidRule(void) {
    const bool sf = useSfBackdoor;
    auto isDefaultRule = [](const FpsRule &rule) { return rule.idle == -1 && rule.active == -1; };
    auto isSfBackdoorRule = [](const FpsRule &rule) { return rule.idle < 20 && rule.active < 20; };
    auto isInvalid = [&](const FpsRule &rule) {
        return isDefaultRule(rule) == false && sf != isSfBackdoorRule(rule);
    };

    if (isInvalid(offscreen)) {
        return "offscreen";
    }
    if (isInvalid(universial)) {
        return "default";
    }
    for (const auto &[name, rule] : rules) {
        if (isInvalid(rule)) {
            return name;
        }
    }

    return {};
}
