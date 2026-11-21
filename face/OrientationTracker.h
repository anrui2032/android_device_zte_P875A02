/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/frameworks/sensorservice/BnEventQueueCallback.h>
#include <aidl/android/frameworks/sensorservice/IEventQueue.h>
#include <aidl/android/frameworks/sensorservice/ISensorManager.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace aidl::android::hardware::biometrics::face {

class OrientationTracker {
  public:
    OrientationTracker() = default;
    ~OrientationTracker();

    OrientationTracker(const OrientationTracker&) = delete;
    OrientationTracker& operator=(const OrientationTracker&) = delete;

    void start();
    void stop();
    int32_t orientation() const { return mOrientation.load(); }

  private:
    class Callback : public ::aidl::android::frameworks::sensorservice::BnEventQueueCallback {
      public:
        explicit Callback(std::atomic<int32_t>* out) : mOut(out) {}
        ndk::ScopedAStatus onEvent(
            const ::aidl::android::hardware::sensors::Event& event) override;

      private:
        std::atomic<int32_t>* mOut;
    };

    bool ensureConnectedLocked();
    void resetLocked();

    std::mutex mLock;
    std::shared_ptr<::aidl::android::frameworks::sensorservice::ISensorManager> mManager;
    std::shared_ptr<::aidl::android::frameworks::sensorservice::IEventQueue> mQueue;
    std::shared_ptr<Callback> mCallback;
    int32_t mSensorHandle = -1;
    bool mEnabled = false;
    std::atomic<int32_t> mOrientation{0};
};

}  // namespace aidl::android::hardware::biometrics::face
