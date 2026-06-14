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

#include "kernel_state_reader.h"
#include "utils/misc.h"
#include <cstring>
#include <dirent.h>
#include <spdlog/spdlog.h>
#include <sys/system_properties.h>

constexpr char MODULE_NAME[] = "KernelStateReader";
constexpr char FB_BLANK_PATH[] = "/sys/class/graphics/fb0/blank";
constexpr char BACKLIGHT_DIR[] = "/sys/class/backlight";
constexpr char DRM_DIR[] = "/sys/class/drm";
constexpr char SYSPROP_DISPLAY_STATE[] = "sys.display_state";

constexpr size_t SYSFS_READ_MAX = 64;

// Try to find a readable file matching pattern: <dir>/<entry>/<filename>
// Returns the full path of the first readable match, or empty string.
static std::string FindFirstReadableInDir(const char *dir, const char *filename) {
    DIR *dp = opendir(dir);
    if (dp == nullptr) {
        return {};
    }

    std::string result;
    struct dirent *ent;
    while ((ent = readdir(dp)) != nullptr) {
        // Skip . and ..
        if (ent->d_name[0] == '.') {
            continue;
        }

        std::string path = std::string(dir) + "/" + ent->d_name + "/" + filename;
        // Probe: try to read
        std::string buf;
        if (ReadFile(path, &buf, SYSFS_READ_MAX) > 0) {
            result = path;
            break;
        }
    }
    closedir(dp);
    return result;
}

// Read a sysfs int value.  Returns -1 on failure.
static int ReadSysfsInt(const std::string &path) {
    std::string buf;
    if (ReadFile(path, &buf, SYSFS_READ_MAX) <= 0) {
        return -1;
    }
    int32_t val = -1;
    if (ParseInt(buf.c_str(), &val)) {
        return val;
    }
    return -1;
}

// Read a sysfs string value, trimmed of trailing whitespace.
static std::string ReadSysfsString(const std::string &path) {
    std::string buf;
    if (ReadFile(path, &buf, SYSFS_READ_MAX) <= 0) {
        return {};
    }
    // Trim trailing whitespace/newline
    while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r' || buf.back() == ' ')) {
        buf.pop_back();
    }
    return buf;
}

KernelStateReader::KernelStateReader(void) : source_(Source::NONE) {
    // Probe in priority order; stop at first success
    if (ProbeFbBlank()) {
        SPDLOG_INFO("{}: using {}", MODULE_NAME, FB_BLANK_PATH);
        return;
    }
    if (ProbeBlPower()) {
        SPDLOG_INFO("{}: using {}", MODULE_NAME, resolvedPath_.c_str());
        return;
    }
    if (ProbeDrmDpms()) {
        SPDLOG_INFO("{}: using {}", MODULE_NAME, resolvedPath_.c_str());
        return;
    }
    if (ProbeSysprop()) {
        SPDLOG_INFO("{}: using sysprop {}", MODULE_NAME, SYSPROP_DISPLAY_STATE);
        return;
    }

    SPDLOG_WARN("{}: no kernel display state source available", MODULE_NAME);
}

KernelStateReader::~KernelStateReader(void) {}

DisplayState KernelStateReader::Read(void) const {
    switch (source_) {
        case Source::FB_BLANK:
            return ReadFbBlank();
        case Source::BL_POWER:
            return ReadBlPower();
        case Source::DRM_DPMS:
            return ReadDrmDpms();
        case Source::SYSPROP:
            return ReadSysprop();
        case Source::NONE:
        default:
            return DisplayState::UNKNOWN;
    }
}

bool KernelStateReader::IsAvailable(void) const { return source_ != Source::NONE; }

bool KernelStateReader::ProbeFbBlank(void) {
    int val = ReadSysfsInt(FB_BLANK_PATH);
    if (val >= 0) {
        source_ = Source::FB_BLANK;
        resolvedPath_ = FB_BLANK_PATH;
        return true;
    }
    return false;
}

bool KernelStateReader::ProbeBlPower(void) {
    auto path = FindFirstReadableInDir(BACKLIGHT_DIR, "bl_power");
    if (!path.empty()) {
        source_ = Source::BL_POWER;
        resolvedPath_ = path;
        return true;
    }
    return false;
}

bool KernelStateReader::ProbeDrmDpms(void) {
    // Look for /sys/class/drm/card0-*/dpms
    DIR *dp = opendir(DRM_DIR);
    if (dp == nullptr) {
        return false;
    }

    struct dirent *ent;
    while ((ent = readdir(dp)) != nullptr) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        // Match card0-* connectors (not just card0)
        if (strncmp(ent->d_name, "card0-", 6) != 0) {
            continue;
        }

        std::string path = std::string(DRM_DIR) + "/" + ent->d_name + "/dpms";
        auto val = ReadSysfsString(path);
        if (!val.empty()) {
            source_ = Source::DRM_DPMS;
            resolvedPath_ = path;
            closedir(dp);
            return true;
        }
    }
    closedir(dp);
    return false;
}

bool KernelStateReader::ProbeSysprop(void) {
    char buf[PROP_VALUE_MAX + 1];
    int len = __system_property_get(SYSPROP_DISPLAY_STATE, buf);
    if (len > 0) {
        source_ = Source::SYSPROP;
        return true;
    }
    return false;
}

DisplayState KernelStateReader::ReadFbBlank(void) const {
    // fb0/blank: 0 = unblanked (screen ON), 1 = blanked (screen OFF)
    int val = ReadSysfsInt(resolvedPath_);
    if (val < 0) {
        return DisplayState::UNKNOWN;
    }
    return (val == 0) ? DisplayState::ON : DisplayState::OFF;
}

DisplayState KernelStateReader::ReadBlPower(void) const {
    // bl_power: 0 = ON, non-zero (typically 4 = FB_BLANK_POWERDOWN) = OFF
    int val = ReadSysfsInt(resolvedPath_);
    if (val < 0) {
        return DisplayState::UNKNOWN;
    }
    return (val == 0) ? DisplayState::ON : DisplayState::OFF;
}

DisplayState KernelStateReader::ReadDrmDpms(void) const {
    // dpms: "On" = ON, "Off" = OFF
    auto val = ReadSysfsString(resolvedPath_);
    if (val.empty()) {
        return DisplayState::UNKNOWN;
    }
    if (val == "On") {
        return DisplayState::ON;
    }
    if (val == "Off") {
        return DisplayState::OFF;
    }
    return DisplayState::UNKNOWN;
}

DisplayState KernelStateReader::ReadSysprop(void) const {
    char buf[PROP_VALUE_MAX + 1];
    int len = __system_property_get(SYSPROP_DISPLAY_STATE, buf);
    if (len <= 0) {
        return DisplayState::UNKNOWN;
    }
    buf[len] = '\0';
    if (strcmp(buf, "on") == 0 || strcmp(buf, "ON") == 0) {
        return DisplayState::ON;
    }
    if (strcmp(buf, "off") == 0 || strcmp(buf, "OFF") == 0) {
        return DisplayState::OFF;
    }
    return DisplayState::UNKNOWN;
}
