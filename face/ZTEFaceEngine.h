/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "FaceEngineLoader.h"

namespace aidl::android::hardware::biometrics::face {

class ZTEFaceEngine {
  public:
    static constexpr int32_t kConfigDisableEyeDetect = 0x1;
    static constexpr int32_t kConfigDisableLiveness = 0x2;

    struct ImageInfo {
        float quality = -1.0f;
        int32_t hasFace = 0;
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };

    static ZTEFaceEngine& instance();

    ZTEFaceEngine(const ZTEFaceEngine&) = delete;
    ZTEFaceEngine& operator=(const ZTEFaceEngine&) = delete;

    bool init(const void* owner, const std::string& folder, bool encrypt, int32_t config = 0);
    void deinit(const void* owner);
    void setAttention(bool enabled);

    int32_t count(const std::string& folder, bool encrypt, int32_t* out);

    int32_t enroll(const uint8_t* nv21, int32_t width, int32_t height, int32_t orientation,
                   int32_t* faceId);
    int32_t authenticate(const uint8_t* nv21, int32_t width, int32_t height, int32_t orientation,
                         int32_t* faceId);

    int32_t imageInfo(ImageInfo* out) const;

    int32_t deleteAll(const std::string& folder, bool encrypt);

  private:
    ZTEFaceEngine() = default;
    ~ZTEFaceEngine();

    int32_t call(bool enroll, const uint8_t* nv21, int32_t width, int32_t height,
                 int32_t orientation, int32_t* faceId);

    mutable std::mutex mLock;
    const void* mOwner = nullptr;
    int64_t mHandle = 0;
    bool mRequestedEncrypt = false;
    bool mEncrypt = false;
    int32_t mConfig = 0;
    bool mAttention = true;
    char mFolderBuf[200] = {};
    std::string mFolder;
    FaceEngineLoader mLoader;
};

}  // namespace aidl::android::hardware::biometrics::face
