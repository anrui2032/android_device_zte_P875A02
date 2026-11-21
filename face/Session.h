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
#include "OrientationTracker.h"
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

    void onFrame(uint64_t gen, const uint8_t* nv21, int32_t width, int32_t height,
                 CameraSource::Orientation orientation);
    void onCameraError(uint64_t gen, int error);
    void processFrame(uint64_t gen, const uint8_t* nv21, int32_t width, int32_t height,
                      CameraSource::Orientation orientation);
    void workerLoop();
    void stopWorker();
    void finishAuthTimeout(uint64_t gen);
    void sendAuthFrame(int32_t ret);
    void sendEnrollFrame(int32_t ret);
    std::shared_ptr<common::ICancellationSignal> makeCancel(uint64_t gen);
    void cancelOp(uint64_t gen);
    void reportError(Error e, int32_t vendor = 0);
    void scheduleCameraStop(uint64_t gen);
    void stopCamera();
    void joinStopThread();
    void rollbackUncommittedEnroll();
    bool loadAttention();
    bool saveAttention(bool enabled);
    void applyAttention();

    const int32_t mSensorId;
    const int32_t mUserId;
    const std::string mFolder;
    std::shared_ptr<ISessionCallback> mCb;

    mutable std::mutex mLock;
    CameraSource mCamera;
    OrientationTracker mOrientationTracker;
    std::atomic<Op> mOp{Op::NONE};
    std::atomic<bool> mDone{true};
    std::atomic<bool> mClosed{false};
    std::atomic<uint64_t> mOpGen{0};
    std::atomic<int64_t> mChallenge{0};
    std::atomic<int64_t> mAuthDeadlineMs{0};
    std::atomic<int32_t> mAuthCompareFailCount{0};
    std::atomic<int32_t> mAuthNoFaceCount{0};
    std::atomic<int32_t> mAuthOtherCount{0};
    std::mutex mStopMutex;
    std::condition_variable mStopCv;
    bool mCancelStopDelay = false;
    std::thread mStopThread;
    std::thread mWorker;
    std::mutex mFrameMutex;
    std::condition_variable mFrameCv;
    struct Frame {
        uint64_t gen = 0;
        std::vector<uint8_t> nv21;
        int32_t width = 0;
        int32_t height = 0;
        CameraSource::Orientation orientation = CameraSource::Orientation::UP;
    };
    std::unique_ptr<Frame> mPendingFrame;
    bool mWorkerStop = false;
    uint64_t mEnrollGen = 0;
    int64_t mEnrollFirstFrameMs = 0;
    AcquiredInfo mEnrollHelpInfo = AcquiredInfo::UNKNOWN;
    int32_t mEnrollHelpStreak = 0;
    int32_t mEnrollRemaining = 0;
    int32_t mEnrollFinalDelay = 0;
    int32_t mEnrollFaceId = 0;
    bool mEnrollSucceeded = false;
    std::atomic<bool> mEnrollUncommitted{false};
    std::atomic<bool> mAttention{true};
};

}  // namespace aidl::android::hardware::biometrics::face
