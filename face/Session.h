/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/face/BnSession.h>
#include <aidl/android/hardware/biometrics/face/ISessionCallback.h>
#include <aidl/android/hardware/common/NativeHandle.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "CameraSource.h"
#include "ZTEFaceEngine.h"

namespace aidl::android::hardware::biometrics::face {

class Session : public BnSession {
  public:
    Session(int32_t sensorId, int32_t userId, std::shared_ptr<ISessionCallback> cb);
    ~Session() override;

    ndk::ScopedAStatus generateChallenge() override;
    ndk::ScopedAStatus revokeChallenge(int64_t challenge) override;
    ndk::ScopedAStatus getEnrollmentConfig(EnrollmentType,
                                           std::vector<EnrollmentStageConfig>* out) override;
    ndk::ScopedAStatus enroll(
        const keymaster::HardwareAuthToken& hat, EnrollmentType type,
        const std::vector<Feature>& features,
        const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
        std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus authenticate(int64_t operationId,
                                    std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus enumerateEnrollments() override;
    ndk::ScopedAStatus removeEnrollments(const std::vector<int32_t>& ids) override;
    ndk::ScopedAStatus getFeatures() override;
    ndk::ScopedAStatus setFeature(const keymaster::HardwareAuthToken& hat, Feature feature,
                                  bool enabled) override;
    ndk::ScopedAStatus getAuthenticatorId() override;
    ndk::ScopedAStatus invalidateAuthenticatorId() override;
    ndk::ScopedAStatus resetLockout(const keymaster::HardwareAuthToken& hat) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus authenticateWithContext(
        int64_t operationId, const common::OperationContext&,
        std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus enrollWithContext(
        const keymaster::HardwareAuthToken& hat, EnrollmentType type,
        const std::vector<Feature>& features,
        const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
        const common::OperationContext& context,
        std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus detectInteractionWithContext(
        const common::OperationContext&, std::shared_ptr<common::ICancellationSignal>* out) override;
    ndk::ScopedAStatus onContextChanged(const common::OperationContext&) override;
    ndk::ScopedAStatus enrollWithOptions(const FaceEnrollOptions& options,
                                         std::shared_ptr<common::ICancellationSignal>* out) override;

  private:
    enum class Op { NONE, ENROLL, AUTH };
    class FnCancel : public common::BnCancellationSignal {
      public:
        explicit FnCancel(std::function<void()> fn) : mFn(std::move(fn)) {}
        ndk::ScopedAStatus cancel() override {
            if (mFn) {
                mFn();
            }
            return ndk::ScopedAStatus::ok();
        }

      private:
        std::function<void()> mFn;
    };

    void onFrame(const uint8_t* nv21, int32_t width, int32_t height,
                 CameraSource::Orientation orientation);
    void processFrame(const uint8_t* nv21, int32_t width, int32_t height,
                      CameraSource::Orientation orientation);
    void workerLoop();
    void stopWorker();
    void sendAuthFrame(int32_t ret);
    void sendEnrollFrame(int32_t ret);
    std::shared_ptr<common::ICancellationSignal> makeCancel();
    void reportError(Error e, int32_t vendor = 0);
    void scheduleCameraStop();
    void stopCamera();
    int64_t authenticatorId() const;

    const int32_t mSensorId;
    const int32_t mUserId;
    const std::string mFolder;
    std::shared_ptr<ISessionCallback> mCb;

    mutable std::mutex mLock;
    ZTEFaceEngine mEngine;
    CameraSource mCamera;
    std::atomic<Op> mOp{Op::NONE};
    std::atomic<bool> mDone{false};
    std::atomic<int64_t> mChallenge{0};
    std::thread mStopThread;
    std::thread mWorker;
    std::mutex mFrameMutex;
    std::condition_variable mFrameCv;
    struct Frame {
        std::vector<uint8_t> nv21;
        int32_t width = 0;
        int32_t height = 0;
        CameraSource::Orientation orientation = CameraSource::Orientation::UP;
    };
    std::unique_ptr<Frame> mPendingFrame;
    bool mWorkerStop = false;
    int32_t mEnrollRemaining = 0;
    int32_t mEnrollFinalDelay = 0;
    int32_t mEnrollFaceId = 0;
    bool mEnrollSucceeded = false;
};

}  // namespace aidl::android::hardware::biometrics::face
