/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android/native_window.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aidl::android::hardware::biometrics::face {

class CameraSource {
  public:
    enum class Orientation : int32_t {
        UP = 1,
        LEFT = 2,
        DOWN = 4,
        RIGHT = 8,
    };

    using FrameCallback = std::function<void(const uint8_t* nv21, int32_t width, int32_t height,
                                             Orientation orientation)>;

    CameraSource() = default;
    ~CameraSource();

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    bool start(ANativeWindow* preview, FrameCallback cb);
    void stop();

  private:
    static void onDeviceDisconnected(void* ctx, ACameraDevice* dev);
    static void onDeviceError(void* ctx, ACameraDevice* dev, int error);
    static void onSessionReady(void* ctx, ACameraCaptureSession* session);
    static void onSessionClosed(void* ctx, ACameraCaptureSession* session);
    static void onImage(void* ctx, AImageReader* reader);
    static void onPreviewImage(void* ctx, AImageReader* reader);

    void handleImage(AImageReader* reader);
    void renderPreview(const uint8_t* nv21, int32_t width, int32_t height);
    void closeLocked();

    std::atomic<bool> mRunning{false};
    std::mutex mLock;
    FrameCallback mCallback;
    Orientation mOrientation = Orientation::UP;
    int32_t mSensorOrientation = 0;

    void* mManager = nullptr;
    void* mDevice = nullptr;
    void* mSession = nullptr;
    void* mReader = nullptr;
    void* mPreviewReader = nullptr;
    void* mOutputs = nullptr;
    void* mOutput = nullptr;
    void* mPreviewOutput = nullptr;
    void* mRequest = nullptr;
    void* mTarget = nullptr;
    void* mPreviewTarget = nullptr;
    ANativeWindow* mRenderWindow = nullptr;
    std::vector<uint8_t> mNv21;
};

}  // namespace aidl::android::hardware::biometrics::face
