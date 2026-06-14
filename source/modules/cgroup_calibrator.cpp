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

#include "cgroup_calibrator.h"
#include <cmath>

CgroupCalibrator::CgroupCalibrator(void) : CgroupCalibrator(Config{}) {}

CgroupCalibrator::CgroupCalibrator(Config cfg)
    : config_(cfg),
      emaBaseline_(static_cast<float>(cfg.absoluteMax) / 2.0f), // conservative default during warmup
      sampleCount_(0),
      frozen_(false) {}

void CgroupCalibrator::ObserveOnScreen(int restrictedCount) {
    if (frozen_) {
        return;
    }

    // Reject obvious outliers during early samples to avoid seeding with bad data
    if (sampleCount_ == 0) {
        emaBaseline_ = static_cast<float>(restrictedCount);
    } else {
        // Exponential moving average: baseline = alpha * new + (1-alpha) * old
        emaBaseline_ = config_.emaAlpha * static_cast<float>(restrictedCount) +
                       (1.0f - config_.emaAlpha) * emaBaseline_;
    }
    sampleCount_++;
}

void CgroupCalibrator::Freeze(void) { frozen_ = true; }

void CgroupCalibrator::Unfreeze(void) { frozen_ = false; }

bool CgroupCalibrator::IsCalibrated(void) const { return sampleCount_ >= config_.warmupSamples; }

int CgroupCalibrator::GetHiThreshold(void) const { return ClampBaseline(emaBaseline_) + config_.hiMargin; }

int CgroupCalibrator::GetLoThreshold(void) const { return ClampBaseline(emaBaseline_) + config_.loMargin; }

int CgroupCalibrator::GetBaseline(void) const { return ClampBaseline(emaBaseline_); }

int CgroupCalibrator::GetSampleCount(void) const { return sampleCount_; }

bool CgroupCalibrator::IsWarmingUp(void) const { return sampleCount_ < config_.warmupSamples; }

int CgroupCalibrator::ClampBaseline(float raw) const {
    int rounded = static_cast<int>(std::round(raw));
    return std::max(config_.absoluteMin, std::min(config_.absoluteMax, rounded));
}
