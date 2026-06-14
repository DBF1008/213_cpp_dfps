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

#include "offscreen_state_fusion.h"
#include <string>

// Reads the kernel-level display power state from sysfs.
//
// Probes multiple sysfs paths at construction time and caches the first one
// that is readable.  Subsequent Read() calls only access that path.
//
// Fallback chain:
//   1. /sys/class/graphics/fb0/blank         (0 = unblanked/ON, non-zero = blanked/OFF)
//   2. /sys/class/backlight/*/bl_power       (0 = ON, non-zero = OFF)
//   3. /sys/class/drm/card0-*/dpms           ("On" = ON, "Off" = OFF)
//   4. getprop sys.display_state             ("on" = ON, "off" = OFF)
//
// If none are available, IsAvailable() returns false and Read() always
// returns DisplayState::UNKNOWN.
class KernelStateReader {
public:
    KernelStateReader(void);
    ~KernelStateReader(void);

    DisplayState Read(void) const;
    bool IsAvailable(void) const;

private:
    enum class Source { FB_BLANK, BL_POWER, DRM_DPMS, SYSPROP, NONE };

    // Probe functions — return true if path is readable and parsed successfully
    bool ProbeFbBlank(void);
    bool ProbeBlPower(void);
    bool ProbeDrmDpms(void);
    bool ProbeSysprop(void);

    // Read from the cached source
    DisplayState ReadFbBlank(void) const;
    DisplayState ReadBlPower(void) const;
    DisplayState ReadDrmDpms(void) const;
    DisplayState ReadSysprop(void) const;

    Source source_;
    std::string resolvedPath_; // Resolved sysfs path (empty for SYSPROP/NONE)
};
