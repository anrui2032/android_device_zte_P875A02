/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHalSession"

#include "Session.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/native_window_aidl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

namespace aidl::android::hardware::biometrics::face {

using aidl::android::hardware::biometrics::common::OperationContext;

namespace {
constexpr char kAttentionPath[] = "/data/vendor/face/attention";
constexpr int64_t kAuthTimeoutMs = 5000;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

Session::Session(int32_t sensorId, int32_t userId, std::shared_ptr<ISessionCallback> cb)
    : mSensorId(sensorId),
      mUserId(userId),
      mFolder("/data/vendor/face/" + std::to_string(userId)),
      mCb(std::move(cb)) {
    LOG(INFO) << "native session sensor=" << mSensorId << " user=" << mUserId
              << " folder=" << mFolder;
    mAttention = loadAttention();
    mEngine.setAttention(mAttention);
    mWorker = std::thread([this] { workerLoop(); });
}

bool Session::loadAttention() {
    std::string content;
    if (::android::base::ReadFileToString(kAttentionPath, &content) && !content.empty()) {
        return content[0] != '0';
    }
    return true;
}

bool Session::saveAttention(bool enabled) {
    const std::string tmp = std::string(kAttentionPath) + ".tmp";
    if (!::android::base::WriteStringToFile(enabled ? "1" : "0", tmp, 0600, getuid(), getgid())) {
        PLOG(ERROR) << "failed to write " << tmp;
        return false;
    }
    if (::rename(tmp.c_str(), kAttentionPath) != 0) {
        PLOG(ERROR) << "failed to rename " << tmp;
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

void Session::applyAttention() {
    mAttention = loadAttention();
    mEngine.setAttention(mAttention);
}

Session::~Session() {
    stopCamera();
    stopWorker();
    if (mStopThread.joinable()) {
        mStopThread.join();
    }
    mEngine.deinit();
}

std::shared_ptr<common::ICancellationSignal> Session::makeCancel() {
    return ndk::SharedRefBase::make<FnCancel>([this] {
        mDone = true;
        mCamera.stop();
    });
}

void Session::reportError(Error e, int32_t vendor) {
    mDone = true;
    mOp = Op::NONE;
    if (mCb) {
        mCb->onError(e, vendor);
    }
}

void Session::stopCamera() {
    if (mStopThread.joinable()) {
        mStopThread.join();
    }
    mCamera.stop();
}

void Session::scheduleCameraStop() {
    if (mStopThread.joinable()) {
        mStopThread.join();
    }
    mStopThread = std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        mCamera.stop();
    });
}

int64_t Session::authenticatorId() const {
    return static_cast<int64_t>(std::hash<std::string>{}(mFolder));
}

ndk::ScopedAStatus Session::generateChallenge() {
    if (mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    std::random_device rd;
    mChallenge = (static_cast<int64_t>(rd()) << 32) ^ rd();
    mCb->onChallengeGenerated(mChallenge.load());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    mCb->onChallengeRevoked(challenge);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getEnrollmentConfig(EnrollmentType,
                                                std::vector<EnrollmentStageConfig>* out) {
    out->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(
    const keymaster::HardwareAuthToken&, EnrollmentType, const std::vector<Feature>&,
    const std::optional<::aidl::android::hardware::common::NativeHandle>&,
    std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel();
    if (mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 16)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    applyAttention();

    stopCamera();
    mDone = false;
    mOp = Op::ENROLL;
    mEnrollSucceeded = false;
    mEnrollFaceId = 0;
    mEnrollRemaining = 0;
    mEnrollFinalDelay = 0;
    if (!mCamera.start(nullptr, [this](const uint8_t* d, int32_t w, int32_t h,
                                       CameraSource::Orientation o) { onFrame(d, w, h, o); })) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<common::ICancellationSignal>* out) {
    LOG(INFO) << "authenticate called user=" << mUserId;
    *out = makeCancel();
    if (mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 0)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    (void)operationId;
    applyAttention();
    LOG(INFO) << "authenticate attention=" << mAttention;
    stopCamera();
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mDone = false;
        mAuthCompareFailed = false;
        mAuthDeadlineMs = nowMs() + kAuthTimeoutMs;
        mOp = Op::AUTH;
    }
    mFrameCv.notify_one();
    if (!mCamera.start(nullptr, [this](const uint8_t* d, int32_t w, int32_t h,
                                       CameraSource::Orientation o) { onFrame(d, w, h, o); })) {
        reportError(Error::HW_UNAVAILABLE);
    }
    return ndk::ScopedAStatus::ok();
}

void Session::onFrame(const uint8_t* nv21, int32_t width, int32_t height,
                      CameraSource::Orientation orientation) {
    if (mDone.load() || mOp.load() == Op::NONE) {
        return;
    }

    auto frame = std::make_unique<Frame>();
    frame->width = width;
    frame->height = height;
    frame->orientation = orientation;
    frame->nv21.assign(nv21, nv21 + static_cast<size_t>(width) * height * 3 / 2);

    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mPendingFrame = std::move(frame);
    }
    mFrameCv.notify_one();
}

void Session::workerLoop() {
    for (;;) {
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(mFrameMutex);
            if (mOp.load() == Op::AUTH && !mDone.load()) {
                const auto deadline = std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(mAuthDeadlineMs.load()));
                mFrameCv.wait_until(lock, deadline, [this] { return mWorkerStop || mPendingFrame; });
            } else {
                mFrameCv.wait(lock, [this] { return mWorkerStop || mPendingFrame; });
            }
            if (mWorkerStop) {
                return;
            }
            if (!mPendingFrame) {
                lock.unlock();
                if (mOp.load() == Op::AUTH && !mDone.load() && nowMs() >= mAuthDeadlineMs.load()) {
                    finishAuthTimeout();
                }
                continue;
            }
            frame = std::move(*mPendingFrame);
            mPendingFrame.reset();
        }
        processFrame(frame.nv21.data(), frame.width, frame.height, frame.orientation);
    }
}

void Session::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mWorkerStop = true;
    }
    mFrameCv.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void Session::processFrame(const uint8_t* nv21, int32_t width, int32_t height,
                           CameraSource::Orientation orientation) {
    if (mDone.load() || mOp.load() == Op::NONE) {
        return;
    }
    const Op op = mOp.load();
    int32_t faceId = (op == Op::AUTH) ? 0 : -1;
    if (op == Op::ENROLL && mEnrollSucceeded) {
        if (mEnrollRemaining > 1) {
            mEnrollRemaining = std::max(1, mEnrollRemaining - 5);
            mCb->onEnrollmentProgress(0, mEnrollRemaining);
        } else {
            if (++mEnrollFinalDelay >= 15) {
                mDone = true;
                mCb->onEnrollmentProgress(mEnrollFaceId, 0);
                scheduleCameraStop();
            }
        }
        return;
    }
    if (op == Op::ENROLL) {
        const int32_t ret =
            mEngine.enroll(nv21, width, height, static_cast<int32_t>(orientation), &faceId);
        LOG(INFO) << "enroll frame " << width << "x" << height << " ret=" << ret
                  << " faceId=" << faceId;
        if (ret == 0) {
            mEnrollSucceeded = true;
            mEnrollFaceId = faceId;
            mEnrollRemaining = 100;
            mCb->onEnrollmentProgress(0, mEnrollRemaining);
        } else if (ret > 0) {
            sendEnrollFrame(ret);
            ZTEFaceEngine::ImageInfo info;
            if (mEngine.imageInfo(&info) == 0) {
                LOG(INFO) << "enroll retry quality=" << info.quality << " hasFace=" << info.hasFace
                          << " rect=" << info.left << "," << info.top << "," << info.right << ","
                          << info.bottom;
            } else {
                LOG(INFO) << "enroll retry";
            }
        } else {
            mDone = true;
            reportError(Error::UNABLE_TO_PROCESS, ret);
        }
    } else if (op == Op::AUTH) {
        if (nowMs() >= mAuthDeadlineMs.load()) {
            finishAuthTimeout();
            return;
        }
        const int32_t ret =
            mEngine.authenticate(nv21, width, height, static_cast<int32_t>(orientation), &faceId);
        LOG(INFO) << "auth frame ret=" << ret << " faceId=" << faceId
                  << " orientation=" << static_cast<int32_t>(orientation);
        if (ret == 0) {
            mDone = true;
            mCb->onAuthenticationSucceeded(faceId, keymaster::HardwareAuthToken{});
            scheduleCameraStop();
        } else if (ret > 0) {
            if (ret == 201) {
                mAuthCompareFailed = true;
            }
            sendAuthFrame(ret);
            ZTEFaceEngine::ImageInfo info;
            if (mEngine.imageInfo(&info) == 0) {
                LOG(INFO) << "auth retry quality=" << info.quality << " hasFace=" << info.hasFace
                          << " rect=" << info.left << "," << info.top << "," << info.right << ","
                          << info.bottom;
            } else {
                LOG(INFO) << "auth retry";
            }
        } else {
            mDone = true;
            reportError(Error::UNABLE_TO_PROCESS, ret);
        }
    }
}

void Session::finishAuthTimeout() {
    if (mDone.exchange(true)) {
        return;
    }
    if (mAuthCompareFailed.load()) {
        LOG(INFO) << "auth window ended: compare failure";
        mCb->onAuthenticationFailed();
    } else {
        LOG(INFO) << "auth window ended: no compare failure";
        reportError(Error::TIMEOUT);
    }
    scheduleCameraStop();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel();
    reportError(Error::UNABLE_TO_PROCESS);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    if (mUserId != 0) {
        mCb->onEnrollmentsEnumerated({});
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 0)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    int32_t count = 0;
    const int32_t ret = mEngine.count(&count);
    if (ret != 0) {
        reportError(Error::UNABLE_TO_PROCESS, ret);
        return ndk::ScopedAStatus::ok();
    }
    if (count > 0) {
        mCb->onEnrollmentsEnumerated({0});
    } else {
        mCb->onEnrollmentsEnumerated({});
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& ids) {
    std::string id_list;
    for (int32_t id : ids) {
        if (!id_list.empty()) {
            id_list += ",";
        }
        id_list += std::to_string(id);
    }
    LOG(INFO) << "removeEnrollments entered user=" << mUserId << " ids=[" << id_list << "]";
    if (mUserId != 0) {
        reportError(Error::UNABLE_TO_REMOVE);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 0)) {
        reportError(Error::UNABLE_TO_REMOVE);
        return ndk::ScopedAStatus::ok();
    }
    int32_t count_before = -1;
    mEngine.count(&count_before);
    const int32_t ret = mEngine.deleteAll();
    int32_t count_after = -1;
    mEngine.count(&count_after);
    LOG(INFO) << "removeEnrollments ret=" << ret << " count=" << count_before << "->" << count_after;
    if (ret != 0) {
        reportError(Error::UNABLE_TO_REMOVE, ret);
        return ndk::ScopedAStatus::ok();
    }
    mCb->onEnrollmentsRemoved(ids);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getFeatures() {
    applyAttention();
    std::vector<Feature> features;
    if (mAttention) {
        features.push_back(Feature::REQUIRE_ATTENTION);
    }
    LOG(INFO) << "getFeatures attention=" << mAttention;
    mCb->onFeaturesRetrieved(features);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const keymaster::HardwareAuthToken&, Feature feature,
                                       bool enabled) {
    if (feature != Feature::REQUIRE_ATTENTION) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!saveAttention(enabled)) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    mAttention = enabled;
    mEngine.setAttention(enabled);
    LOG(INFO) << "setFeature attention=" << enabled;
    mCb->onFeatureSet(feature);
    return ndk::ScopedAStatus::ok();
}

namespace {
bool isQuietRetryCode(int32_t ret) {
    switch (ret) {
        case 1:
        case 12:
        case 101:
            return true;
        default:
            return false;
    }
}

AcquiredInfo acquiredInfoFor(int32_t ret) {
    switch (ret) {
        case 1:
        case 8:
            return AcquiredInfo::NOT_DETECTED;
        case 2:
            return AcquiredInfo::POOR_GAZE;
        case 3:
            return AcquiredInfo::TOO_BRIGHT;
        case 4:
            return AcquiredInfo::TOO_DARK;
        case 5:
            return AcquiredInfo::TOO_BRIGHT;
        case 6:
        case 7:
        case 15:
            return AcquiredInfo::FACE_OBSCURED;
        case 9:
            return AcquiredInfo::POOR_GAZE;
        case 10:
            return AcquiredInfo::TOO_MUCH_MOTION;
        case 13:
            return AcquiredInfo::TOO_FAR;
        case 14:
            return AcquiredInfo::TOO_CLOSE;
        default:
            return AcquiredInfo::INSUFFICIENT;
    }
}
}  // namespace

void Session::sendAuthFrame(int32_t ret) {
    if (isQuietRetryCode(ret)) {
        return;
    }
    const AcquiredInfo info = acquiredInfoFor(ret);
    LOG(INFO) << "auth acquired ret=" << ret << " info=" << static_cast<int32_t>(info);
    if (info == AcquiredInfo::GOOD) {
        return;
    }
    AuthenticationFrame frame;
    frame.data.acquiredInfo = info;
    mCb->onAuthenticationFrame(frame);
}

void Session::sendEnrollFrame(int32_t ret) {
    if (isQuietRetryCode(ret)) {
        return;
    }
    const AcquiredInfo info = acquiredInfoFor(ret);
    LOG(INFO) << "enroll acquired ret=" << ret << " info=" << static_cast<int32_t>(info);
    if (info == AcquiredInfo::GOOD) {
        return;
    }
    EnrollmentFrame frame;
    frame.stage = EnrollmentStage::FIRST_FRAME_RECEIVED;
    frame.data.acquiredInfo = info;
    mCb->onEnrollmentFrame(frame);
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    mCb->onAuthenticatorIdRetrieved(authenticatorId());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    mCb->onAuthenticatorIdInvalidated(authenticatorId() ^ 0x5a5a5a5a5a5a5a5aLL);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const keymaster::HardwareAuthToken&) {
    mCb->onLockoutCleared();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::close() {
    stopCamera();
    stopWorker();
    if (mStopThread.joinable()) {
        mStopThread.join();
    }
    mEngine.deinit();
    mCb->onSessionClosed();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
    int64_t operationId, const OperationContext&,
    std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(
    const keymaster::HardwareAuthToken& hat, EnrollmentType type,
    const std::vector<Feature>& features,
    const std::optional<::aidl::android::hardware::common::NativeHandle>& previewSurface,
    const OperationContext&, std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, type, features, previewSurface, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
    const OperationContext&, std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onContextChanged(const OperationContext&) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enrollWithOptions(const FaceEnrollOptions& options,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel();
    if (mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 16)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    applyAttention();

    stopCamera();
    ANativeWindow* window = nullptr;
    if (options.surfacePreview.has_value()) {
        window = options.surfacePreview->get();
        if (window == nullptr) {
            reportError(Error::UNABLE_TO_PROCESS);
            return ndk::ScopedAStatus::ok();
        }
    }

    mDone = false;
    mOp = Op::ENROLL;
    mEnrollSucceeded = false;
    mEnrollFaceId = 0;
    mEnrollRemaining = 0;
    mEnrollFinalDelay = 0;
    LOG(INFO) << "enroll options preview=" << (window != nullptr);
    if (!mCamera.start(window, [this](const uint8_t* d, int32_t w, int32_t h,
                                      CameraSource::Orientation o) { onFrame(d, w, h, o); })) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
