/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace aidl::android::hardware::biometrics::face {

struct ZTEFaceRecInitParam {
    const char* folder;
    int32_t config;
    uint8_t use_encryption;
    uint8_t pad[7];
    uint64_t reserved;
    int32_t log_level;
    int32_t pad2;
};
static_assert(sizeof(ZTEFaceRecInitParam) == 0x28, "init param");

struct ZTERect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct ZTEFaceRecImage {
    uint8_t* data;
    int32_t pixel_format;
    int32_t orientation;
    ZTERect* roi;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t stride_chroma;
};
static_assert(sizeof(ZTEFaceRecImage) == 0x28, "image");

struct ZTEImageInfoHead {
    float quality;
    int32_t has_face;
    ZTERect rect;
};

class FaceEngineLoader {
  public:
    using InitFn = int64_t (*)(const ZTEFaceRecInitParam*, int32_t*);
    using DeinitFn = void (*)(int64_t);
    using CountFn = int32_t (*)(const char*, int32_t*, int32_t, void*);
    using EnrollFn = int32_t (*)(int64_t, const ZTEFaceRecImage*, int32_t*, int32_t);
    using UnlockFn = int32_t (*)(int64_t, const ZTEFaceRecImage*, int32_t*, int32_t);
    using ImageInfoFn = int32_t (*)(int64_t, void*);
    using DeleteAllFn = int32_t (*)(const char*, int64_t, int32_t, void*);

    FaceEngineLoader() = default;
    ~FaceEngineLoader();

    FaceEngineLoader(const FaceEngineLoader&) = delete;
    FaceEngineLoader& operator=(const FaceEngineLoader&) = delete;

    bool load();
    void unload();

    InitFn init = nullptr;
    DeinitFn deinit = nullptr;
    CountFn getFaceIdCount = nullptr;
    EnrollFn enroll = nullptr;
    UnlockFn unlock = nullptr;
    ImageInfoFn getImageInfo = nullptr;
    DeleteAllFn deleteAll = nullptr;

  private:
    void* mLib = nullptr;
};

}  // namespace aidl::android::hardware::biometrics::face
