/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHal"

#include "Face.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::biometrics::face {

namespace {
constexpr int32_t kFaceSensorId = 10;
}  // namespace

ndk::ScopedAStatus Face::getSensorProps(std::vector<SensorProps>* return_val) {
    SensorProps p;
    p.commonProps.sensorId = kFaceSensorId;
    p.commonProps.sensorStrength = common::SensorStrength::CONVENIENCE;
    p.commonProps.maxEnrollmentsPerUser = 1;
    p.sensorType = FaceSensorType::RGB;
    p.halControlsPreview = true;
    p.previewDisplayId = 0;
    p.enrollPreviewWidth = 480;
    p.enrollPreviewHeight = 480;
    p.enrollTranslationX = 0.0f;
    p.enrollTranslationY = 0.0f;
    p.enrollPreviewScale = 1.0f;
    p.supportsDetectInteraction = false;
    return_val->push_back(std::move(p));
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Face::createSession(int32_t sensorId, int32_t userId,
                                       const std::shared_ptr<ISessionCallback>& cb,
                                       std::shared_ptr<ISession>* return_val) {
    LOG(INFO) << "createSession sensor=" << sensorId << " user=" << userId;
    auto s = ndk::SharedRefBase::make<Session>(sensorId, userId, cb);
    *return_val = s;
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
