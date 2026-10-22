/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHalEngine"

#include "ZTEFaceEngine.h"

#include <android-base/logging.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>

namespace aidl::android::hardware::biometrics::face {

namespace {

constexpr int32_t kPixelFormatNv21 = 2;

}  // namespace

ZTEFaceEngine::~ZTEFaceEngine() {
    deinit();
}

bool ZTEFaceEngine::init(const std::string& folder, bool encrypt, int32_t config) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mLoader.load()) {
        LOG(ERROR) << "failed to load ZTE face engine";
        return false;
    }
    if (mHandle != 0) {
        if (mConfig == config) {
            return true;
        }
        mLoader.deinit(mHandle);
        mHandle = 0;
    }
    if (folder.empty() || folder.size() > 199) {
        return false;
    }
    if (::mkdir(folder.c_str(), 0700) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "mkdir " << folder;
        return false;
    }

    mFolder = folder;
    ZTEFaceRecInitParam param{};
    param.folder = mFolder.c_str();
    param.config = config;
    param.use_encryption = encrypt ? 1 : 0;
    param.log_level = 0;

    int32_t status = -1;
    mHandle = mLoader.init(&param, &status);
    if (mHandle == 0 || status != 0) {
        LOG(ERROR) << "zte_FaceRec_init status=" << status << " folder=" << folder;
        mHandle = 0;
        return false;
    }
    mEncrypt = encrypt;
    mConfig = config;
    LOG(INFO) << "engine ready folder=" << folder << " encrypt=" << encrypt << " config=" << config;
    return true;
}

void ZTEFaceEngine::deinit() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHandle != 0) {
        mLoader.deinit(mHandle);
        mHandle = 0;
    }
}

int32_t ZTEFaceEngine::count(int32_t* out) const {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHandle == 0 || out == nullptr) {
        return -1;
    }
    return mLoader.getFaceIdCount(mFolder.c_str(), out, mEncrypt ? 1 : 0, nullptr);
}

int32_t ZTEFaceEngine::call(bool enroll, const uint8_t* nv21, int32_t width, int32_t height,
                            int32_t orientation, int32_t* faceId) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHandle == 0 || nv21 == nullptr || width <= 0 || height <= 0) {
        return -1;
    }

    ZTEFaceRecImage image{};
    image.data = const_cast<uint8_t*>(nv21);
    image.pixel_format = kPixelFormatNv21;
    image.orientation = orientation;
    image.roi = nullptr;
    image.width = width;
    image.height = height;
    image.stride = width;
    image.stride_chroma = width;
    return enroll ? mLoader.enroll(mHandle, &image, faceId, mConfig)
                  : mLoader.unlock(mHandle, &image, faceId, 1);
}

int32_t ZTEFaceEngine::enroll(const uint8_t* nv21, int32_t width, int32_t height,
                              int32_t orientation, int32_t* faceId) {
    return call(true, nv21, width, height, orientation, faceId);
}

int32_t ZTEFaceEngine::authenticate(const uint8_t* nv21, int32_t width, int32_t height,
                                    int32_t orientation, int32_t* faceId) {
    return call(false, nv21, width, height, orientation, faceId);
}

int32_t ZTEFaceEngine::deleteAll() {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHandle == 0) {
        return -1;
    }
    return mLoader.deleteAll(mFolder.c_str(), mHandle, mEncrypt ? 1 : 0, nullptr);
}

int32_t ZTEFaceEngine::imageInfo(ImageInfo* out) const {
    if (out == nullptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mLock);
    if (mHandle == 0) {
        return -1;
    }
    alignas(8) uint8_t raw[0x82c] = {};
    const int32_t ret = mLoader.getImageInfo(mHandle, raw);
    if (ret != 0) {
        return ret;
    }
    ZTEImageInfoHead head{};
    std::memcpy(&head, raw, sizeof(head));
    out->quality = head.quality;
    out->hasFace = head.has_face;
    out->left = head.rect.left;
    out->top = head.rect.top;
    out->right = head.rect.right;
    out->bottom = head.rect.bottom;
    return 0;
}

}  // namespace aidl::android::hardware::biometrics::face
