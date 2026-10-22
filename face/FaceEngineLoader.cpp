/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHalLoader"

#include "FaceEngineLoader.h"

#include <android-base/logging.h>
#include <dlfcn.h>

namespace aidl::android::hardware::biometrics::face {

namespace {

constexpr char kLibrary[] = "libzteFaceRecognition.so";

}  // namespace

FaceEngineLoader::~FaceEngineLoader() {
    unload();
}

bool FaceEngineLoader::load() {
    if (mLib != nullptr) {
        return true;
    }

    void* lib = dlopen(kLibrary, RTLD_NOW);
    if (lib == nullptr) {
        LOG(ERROR) << "dlopen " << kLibrary << " failed: " << dlerror();
        return false;
    }

    init = reinterpret_cast<InitFn>(dlsym(lib, "zte_FaceRec_init"));
    deinit = reinterpret_cast<DeinitFn>(dlsym(lib, "zte_FaceRec_deinit"));
    getFaceIdCount = reinterpret_cast<CountFn>(dlsym(lib, "zte_FaceRec_get_faceidCnt"));
    enroll = reinterpret_cast<EnrollFn>(dlsym(lib, "zte_FaceRec_enroll"));
    unlock = reinterpret_cast<UnlockFn>(dlsym(lib, "zte_FaceRec_unlock"));
    getImageInfo = reinterpret_cast<ImageInfoFn>(dlsym(lib, "zte_FaceRec_getImageInfo"));
    deleteAll = reinterpret_cast<DeleteAllFn>(dlsym(lib, "zte_FaceRec_delete_all_face"));

    mLib = lib;
    LOG(INFO) << "loaded " << kLibrary;
    return true;
}

void FaceEngineLoader::unload() {
    if (mLib != nullptr) {
        dlclose(mLib);
        mLib = nullptr;
    }

    init = nullptr;
    deinit = nullptr;
    getFaceIdCount = nullptr;
    enroll = nullptr;
    unlock = nullptr;
    getImageInfo = nullptr;
    deleteAll = nullptr;
}

}  // namespace aidl::android::hardware::biometrics::face
