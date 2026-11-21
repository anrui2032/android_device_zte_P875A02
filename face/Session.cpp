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
    const bool enabled = loadAttention();
    mAttention = enabled;
    mEngine.setAttention(enabled);
}

Session::~Session() {
    mClosed = true;
    mDone = true;
    mOp = Op::NONE;
    stopCamera();
    stopWorker();
    joinStopThread();
    mEngine.deinit();
}

std::shared_ptr<common::ICancellationSignal> Session::makeCancel(uint64_t gen) {
    std::weak_ptr<Session> weak = ref<Session>();
    return ndk::SharedRefBase::make<FnCancel>([weak, gen] {
        if (auto self = weak.lock()) {
            self->cancelOp(gen);
        }
    });
}

void Session::cancelOp(uint64_t gen) {
    if (gen == 0 || mOpGen.load() != gen) {
        return;
    }
    if (mDone.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mOp = Op::NONE;
        mPendingFrame.reset();
    }
    mFrameCv.notify_all();
    stopCamera();
    LOG(INFO) << "operation canceled gen=" << gen;
    if (mCb) {
        mCb->onError(Error::CANCELED, 0);
    }
}

void Session::reportError(Error e, int32_t vendor) {
    mDone = true;
    mOp = Op::NONE;
    if (mCb) {
        mCb->onError(e, vendor);
    }
}

void Session::joinStopThread() {
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(mStopMutex);
        mCancelStopDelay = true;
        toJoin = std::move(mStopThread);
    }
    mStopCv.notify_all();
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void Session::stopCamera() {
    joinStopThread();
    mCamera.stop();
}

void Session::scheduleCameraStop(uint64_t gen) {
    joinStopThread();
    std::lock_guard<std::mutex> lock(mStopMutex);
    mCancelStopDelay = false;
    mStopThread = std::thread([this, gen] {
        std::unique_lock<std::mutex> lk(mStopMutex);
        mStopCv.wait_for(lk, std::chrono::milliseconds(100),
                         [this, gen] { return mCancelStopDelay || mOpGen.load() != gen; });
        const bool shouldStop = !mCancelStopDelay && mOpGen.load() == gen;
        lk.unlock();
        if (shouldStop) {
            mCamera.stop();
        }
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
    std::lock_guard<std::mutex> lock(mLock);
    const uint64_t gen = ++mOpGen;
    *out = makeCancel(gen);
    if (mClosed.load() || mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 16)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    applyAttention();

    stopCamera();
    {
        std::lock_guard<std::mutex> frameLock(mFrameMutex);
        mPendingFrame.reset();
        mDone = false;
        mOp = Op::ENROLL;
    }
    mFrameCv.notify_one();
    if (!mCamera.start(
            nullptr,
            [this, gen](const uint8_t* d, int32_t w, int32_t h, CameraSource::Orientation o) {
                onFrame(gen, d, w, h, o);
            },
            [this, gen](int err) { onCameraError(gen, err); })) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<common::ICancellationSignal>* out) {
    std::lock_guard<std::mutex> lock(mLock);
    LOG(INFO) << "authenticate called user=" << mUserId;
    const uint64_t gen = ++mOpGen;
    *out = makeCancel(gen);
    if (mClosed.load() || mUserId != 0) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    if (!mEngine.init(mFolder, true, 0)) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }
    (void)operationId;
    applyAttention();
    LOG(INFO) << "authenticate attention=" << mAttention.load();
    stopCamera();
    {
        std::lock_guard<std::mutex> frameLock(mFrameMutex);
        mPendingFrame.reset();
        mDone = false;
        mAuthCompareFailCount = 0;
        mAuthNoFaceCount = 0;
        mAuthOtherCount = 0;
        mAuthDeadlineMs = nowMs() + kAuthTimeoutMs;
        mOp = Op::AUTH;
    }
    mFrameCv.notify_one();
    if (!mCamera.start(
            nullptr,
            [this, gen](const uint8_t* d, int32_t w, int32_t h, CameraSource::Orientation o) {
                onFrame(gen, d, w, h, o);
            },
            [this, gen](int err) { onCameraError(gen, err); })) {
        reportError(Error::HW_UNAVAILABLE);
    }
    return ndk::ScopedAStatus::ok();
}

void Session::onCameraError(uint64_t gen, int error) {
    if (mOpGen.load() != gen || mDone.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mOp = Op::NONE;
        mPendingFrame.reset();
    }
    mFrameCv.notify_all();
    LOG(ERROR) << "camera error during op gen=" << gen << " error=" << error;
    reportError(Error::HW_UNAVAILABLE);
    scheduleCameraStop(gen);
}

void Session::onFrame(uint64_t gen, const uint8_t* nv21, int32_t width, int32_t height,
                      CameraSource::Orientation orientation) {
    if (mDone.load() || mOp.load() == Op::NONE || mOpGen.load() != gen) {
        return;
    }

    auto frame = std::make_unique<Frame>();
    frame->gen = gen;
    frame->width = width;
    frame->height = height;
    frame->orientation = orientation;
    frame->nv21.assign(nv21, nv21 + static_cast<size_t>(width) * height * 3 / 2);

    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        if (mDone.load() || mOp.load() == Op::NONE || mOpGen.load() != gen) {
            return;
        }
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
                const uint64_t authGen = mOpGen.load();
                const auto deadline = std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(mAuthDeadlineMs.load()));
                mFrameCv.wait_until(lock, deadline, [this, authGen] {
                    return mWorkerStop || mPendingFrame || mDone.load() ||
                           mOpGen.load() != authGen;
                });
            } else {
                mFrameCv.wait(lock, [this] {
                    return mWorkerStop || mPendingFrame ||
                           (mOp.load() == Op::AUTH && !mDone.load());
                });
            }
            if (mWorkerStop) {
                return;
            }
            if (!mPendingFrame) {
                const uint64_t curGen = mOpGen.load();
                const bool timedOut =
                    mOp.load() == Op::AUTH && !mDone.load() && nowMs() >= mAuthDeadlineMs.load();
                lock.unlock();
                if (timedOut) {
                    finishAuthTimeout(curGen);
                }
                continue;
            }
            frame = std::move(*mPendingFrame);
            mPendingFrame.reset();
        }
        processFrame(frame.gen, frame.nv21.data(), frame.width, frame.height, frame.orientation);
    }
}

void Session::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(mFrameMutex);
        mWorkerStop = true;
        mPendingFrame.reset();
    }
    mFrameCv.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
}

void Session::processFrame(uint64_t gen, const uint8_t* nv21, int32_t width, int32_t height,
                           CameraSource::Orientation orientation) {
    if (mDone.load() || mOp.load() == Op::NONE || mOpGen.load() != gen) {
        return;
    }
    const Op op = mOp.load();
    int32_t faceId = (op == Op::AUTH) ? 0 : -1;
    if (op == Op::ENROLL && mEnrollGen != gen) {
        mEnrollGen = gen;
        mEnrollSucceeded = false;
        mEnrollFaceId = 0;
        mEnrollRemaining = 0;
        mEnrollFinalDelay = 0;
    }
    if (op == Op::ENROLL && mEnrollSucceeded) {
        if (mEnrollRemaining > 1) {
            mEnrollRemaining = std::max(1, mEnrollRemaining - 5);
            mCb->onEnrollmentProgress(0, mEnrollRemaining);
        } else {
            if (++mEnrollFinalDelay >= 15) {
                if (mOpGen.load() == gen && !mDone.exchange(true)) {
                    mOp = Op::NONE;
                    mCb->onEnrollmentProgress(mEnrollFaceId, 0);
                    scheduleCameraStop(gen);
                }
            }
        }
        return;
    }
    if (op == Op::ENROLL) {
        const int32_t ret =
            mEngine.enroll(nv21, width, height, static_cast<int32_t>(orientation), &faceId);
        LOG(INFO) << "enroll frame " << width << "x" << height << " ret=" << ret
                  << " faceId=" << faceId;
        if (mOpGen.load() != gen || mDone.load()) {
            return;
        }
        if (ret == 0) {
            mEnrollSucceeded = true;
            mEnrollFaceId = faceId;
            mEnrollRemaining = 100;
            mCb->onEnrollmentProgress(0, mEnrollRemaining);
        } else if (ret > 0 && ret != 203 && ret != 301) {
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
            if (mOpGen.load() == gen && !mDone.exchange(true)) {
                reportError(Error::UNABLE_TO_PROCESS, ret);
                scheduleCameraStop(gen);
            }
        }
    } else if (op == Op::AUTH) {
        if (nowMs() >= mAuthDeadlineMs.load()) {
            finishAuthTimeout(gen);
            return;
        }
        const int32_t ret =
            mEngine.authenticate(nv21, width, height, static_cast<int32_t>(orientation), &faceId);
        LOG(INFO) << "auth frame ret=" << ret << " faceId=" << faceId
                  << " orientation=" << static_cast<int32_t>(orientation);
        if (mOpGen.load() != gen || mDone.load()) {
            return;
        }
        if (ret == 0) {
            if (mOpGen.load() == gen && !mDone.exchange(true)) {
                mOp = Op::NONE;
                mCb->onAuthenticationSucceeded(faceId, keymaster::HardwareAuthToken{});
                scheduleCameraStop(gen);
            }
        } else if (ret > 0 && ret != 203 && ret != 301) {
            if (ret == 201 || ret == 202) {
                ++mAuthCompareFailCount;
            } else if (ret == 1) {
                ++mAuthNoFaceCount;
            } else {
                ++mAuthOtherCount;
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
            if (mOpGen.load() == gen && !mDone.exchange(true)) {
                reportError(Error::UNABLE_TO_PROCESS, ret);
                scheduleCameraStop(gen);
            }
        }
    }
}

void Session::finishAuthTimeout(uint64_t gen) {
    if (mOpGen.load() != gen || mDone.exchange(true)) {
        return;
    }
    mOp = Op::NONE;
    const int32_t compareFail = mAuthCompareFailCount.load();
    const int32_t noFace = mAuthNoFaceCount.load();
    const int32_t other = mAuthOtherCount.load();
    const bool compareFailed =
        compareFail > 0 && compareFail >= noFace && compareFail >= other;
    if (compareFailed) {
        LOG(INFO) << "auth window ended: compare/liveness failure (fail=" << compareFail
                  << " noFace=" << noFace << " other=" << other << ")";
        mCb->onAuthenticationFailed();
    } else {
        LOG(INFO) << "auth window ended: timeout (fail=" << compareFail << " noFace=" << noFace
                  << " other=" << other << ")";
        reportError(Error::TIMEOUT);
    }
    scheduleCameraStop(gen);
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<common::ICancellationSignal>* out) {
    *out = makeCancel(0);
    reportError(Error::UNABLE_TO_PROCESS);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mClosed.load()) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
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
    std::lock_guard<std::mutex> lock(mLock);
    if (mClosed.load()) {
        reportError(Error::UNABLE_TO_REMOVE);
        return ndk::ScopedAStatus::ok();
    }
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
    std::lock_guard<std::mutex> lock(mLock);
    if (mClosed.load()) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
    applyAttention();
    std::vector<Feature> features;
    if (mAttention.load()) {
        features.push_back(Feature::REQUIRE_ATTENTION);
    }
    LOG(INFO) << "getFeatures attention=" << mAttention.load();
    mCb->onFeaturesRetrieved(features);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setFeature(const keymaster::HardwareAuthToken&, Feature feature,
                                       bool enabled) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mClosed.load()) {
        reportError(Error::UNABLE_TO_PROCESS);
        return ndk::ScopedAStatus::ok();
    }
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
        case 101:  // ZTE_FACE_AUTH_STATUS_DETECT_FRAME_SELECT_FAILED
        case 201:  // ZTE_FACE_AUTH_STATUS_FACE_VERITY_FAILED
        case 202:  // ZTE_FACE_AUTH_STATUS_HACKNESS_FAILED
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
        case 11:
        case 15:
            return AcquiredInfo::FACE_OBSCURED;
        case 7:
            return AcquiredInfo::MOUTH_COVERING_DETECTED;
        case 9:
            return AcquiredInfo::POOR_GAZE;
        case 10:
            return AcquiredInfo::TOO_MUCH_MOTION;
        case 12:
            return AcquiredInfo::INSUFFICIENT;
        case 13:
            return AcquiredInfo::TOO_FAR;
        case 14:
            return AcquiredInfo::TOO_CLOSE;
        case 102:
            return AcquiredInfo::TOO_DIFFERENT;
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
    std::lock_guard<std::mutex> lock(mLock);
    mClosed = true;
    ++mOpGen;
    mDone = true;
    mOp = Op::NONE;
    stopCamera();
    stopWorker();
    joinStopThread();
    mEngine.deinit();
    if (mCb) {
        mCb->onSessionClosed();
    }
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
    std::lock_guard<std::mutex> lock(mLock);
    const uint64_t gen = ++mOpGen;
    *out = makeCancel(gen);
    if (mClosed.load() || mUserId != 0) {
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

    {
        std::lock_guard<std::mutex> frameLock(mFrameMutex);
        mPendingFrame.reset();
        mDone = false;
        mOp = Op::ENROLL;
    }
    mFrameCv.notify_one();
    LOG(INFO) << "enroll options preview=" << (window != nullptr);
    if (!mCamera.start(
            window,
            [this, gen](const uint8_t* d, int32_t w, int32_t h, CameraSource::Orientation o) {
                onFrame(gen, d, w, h, o);
            },
            [this, gen](int err) { onCameraError(gen, err); })) {
        reportError(Error::HW_UNAVAILABLE);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::face
