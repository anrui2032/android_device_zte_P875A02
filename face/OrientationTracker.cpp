/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHalOrientation"

#include "OrientationTracker.h"

#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorType.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>

#include <cmath>
#include <string>

namespace aidl::android::hardware::biometrics::face {

namespace {

using ::aidl::android::frameworks::sensorservice::ISensorManager;
using ::aidl::android::hardware::sensors::Event;
using ::aidl::android::hardware::sensors::SensorInfo;
using ::aidl::android::hardware::sensors::SensorType;

constexpr int32_t kSamplingPeriodUs = 50000;
constexpr float kRadToDeg = 57.29578f;

}  // namespace

ndk::ScopedAStatus OrientationTracker::Callback::onEvent(const Event& event) {
    if (event.payload.getTag() != Event::EventPayload::vec3) {
        return ndk::ScopedAStatus::ok();
    }
    const auto& v = event.payload.get<Event::EventPayload::vec3>();
    const float x = -v.x;
    const float y = -v.y;
    const float z = -v.z;
    const float magnitude = x * x + y * y;
    if (4.0f * magnitude < z * z) {
        return ndk::ScopedAStatus::ok();
    }
    int32_t deg = 90 - static_cast<int32_t>(std::lround(std::atan2(-y, x) * kRadToDeg));
    deg = ((deg % 360) + 360) % 360;
    if (deg > 350 || deg < 10) {
        mOut->store(0);
    } else if (deg > 80 && deg < 100) {
        mOut->store(90);
    } else if (deg > 170 && deg < 190) {
        mOut->store(180);
    } else if (deg > 260 && deg < 280) {
        mOut->store(270);
    }
    return ndk::ScopedAStatus::ok();
}

OrientationTracker::~OrientationTracker() {
    stop();
}

void OrientationTracker::resetLocked() {
    mEnabled = false;
    mSensorHandle = -1;
    mQueue.reset();
    mCallback.reset();
    mManager.reset();
}

bool OrientationTracker::ensureConnectedLocked() {
    if (mManager && mQueue && mSensorHandle >= 0) {
        return true;
    }
    resetLocked();

    const std::string name = std::string(ISensorManager::descriptor) + "/default";
    ndk::SpAIBinder binder(AServiceManager_checkService(name.c_str()));
    if (binder.get() == nullptr) {
        LOG(WARNING) << "sensorservice not available yet, defaulting to portrait";
        return false;
    }
    mManager = ISensorManager::fromBinder(binder);
    if (!mManager) {
        return false;
    }

    SensorInfo info{};
    auto status = mManager->getDefaultSensor(SensorType::ACCELEROMETER, &info);
    if (!status.isOk()) {
        LOG(WARNING) << "getDefaultSensor(ACCELEROMETER) failed: " << status.getDescription();
        resetLocked();
        return false;
    }

    mCallback = ndk::SharedRefBase::make<Callback>(&mOrientation);
    status = mManager->createEventQueue(mCallback, &mQueue);
    if (!status.isOk() || !mQueue) {
        LOG(WARNING) << "createEventQueue failed: " << status.getDescription();
        resetLocked();
        return false;
    }

    mSensorHandle = info.sensorHandle;
    return true;
}

void OrientationTracker::start() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mEnabled) {
        return;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureConnectedLocked()) {
            return;
        }
        auto status = mQueue->enableSensor(mSensorHandle, kSamplingPeriodUs, 0);
        if (status.isOk()) {
            mEnabled = true;
            return;
        }
        LOG(WARNING) << "enableSensor failed: " << status.getDescription() << ", reconnecting";
        resetLocked();
    }
}

void OrientationTracker::stop() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mEnabled) {
        return;
    }
    mEnabled = false;
    if (mQueue && mSensorHandle >= 0) {
        auto status = mQueue->disableSensor(mSensorHandle);
        if (!status.isOk()) {
            LOG(WARNING) << "disableSensor failed: " << status.getDescription();
            resetLocked();
        }
    }
}

}  // namespace aidl::android::hardware::biometrics::face
