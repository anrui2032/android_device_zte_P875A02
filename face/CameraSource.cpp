/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#undef LOG_TAG
#define LOG_TAG "ZTEFaceHalCamera"

#include "CameraSource.h"

#include <android-base/logging.h>
#include <android/hardware_buffer.h>

#include <algorithm>
#include <cstring>

namespace aidl::android::hardware::biometrics::face {
namespace {

constexpr int32_t kWidth = 640;
constexpr int32_t kHeight = 480;
constexpr int32_t kPreviewSize = 480;

CameraSource::Orientation orientationFromSensor(int sensor) {
    switch (sensor) {
        case 0:
            return CameraSource::Orientation::UP;
        case 90:
            return CameraSource::Orientation::LEFT;
        case 180:
            return CameraSource::Orientation::DOWN;
        default:
            return CameraSource::Orientation::RIGHT;
    }
}

std::string chooseFrontCamera(ACameraManager* manager, int* sensorOrientation) {
    ACameraIdList* ids = nullptr;
    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK || ids == nullptr) {
        return {};
    }
    std::string result;
    for (int i = 0; i < ids->numCameras; ++i) {
        ACameraMetadata* chars = nullptr;
        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &chars) !=
            ACAMERA_OK) {
            continue;
        }
        ACameraMetadata_const_entry facing{};
        int32_t lens = -1;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_LENS_FACING, &facing) == ACAMERA_OK &&
            facing.count > 0) {
            lens = facing.data.i32[0];
        }
        ACameraMetadata_const_entry so{};
        int32_t orientation = 0;
        if (ACameraMetadata_getConstEntry(chars, ACAMERA_SENSOR_ORIENTATION, &so) == ACAMERA_OK &&
            so.count > 0) {
            orientation = so.data.i32[0];
        }
        ACameraMetadata_free(chars);
        if (lens == ACAMERA_LENS_FACING_FRONT) {
            result = ids->cameraIds[i];
            *sensorOrientation = orientation;
            break;
        }
    }
    ACameraManager_deleteCameraIdList(ids);
    return result;
}

bool nv21FromImage(AImage* image, std::vector<uint8_t>* out, int32_t* outWidth, int32_t* outHeight) {
    int32_t imageWidth = 0, imageHeight = 0;
    if (AImage_getWidth(image, &imageWidth) != AMEDIA_OK ||
        AImage_getHeight(image, &imageHeight) != AMEDIA_OK || imageWidth <= 0 || imageHeight <= 0) {
        return false;
    }
    AImageCropRect crop{0, 0, imageWidth, imageHeight};
    if (AImage_getCropRect(image, &crop) != AMEDIA_OK || crop.right <= crop.left ||
        crop.bottom <= crop.top) {
        crop = {0, 0, imageWidth, imageHeight};
    }
    const int32_t width = crop.right - crop.left;
    const int32_t height = crop.bottom - crop.top;
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
        return false;
    }
    *outWidth = width;
    *outHeight = height;
    int32_t yRow = 0, uRow = 0, vRow = 0;
    int32_t yPix = 0, uPix = 0, vPix = 0;
    uint8_t* y = nullptr;
    uint8_t* u = nullptr;
    uint8_t* v = nullptr;
    int32_t yLen = 0, uLen = 0, vLen = 0;
    if (AImage_getPlaneRowStride(image, 0, &yRow) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 0, &yPix) != AMEDIA_OK ||
        AImage_getPlaneData(image, 0, &y, &yLen) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 1, &uRow) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 1, &uPix) != AMEDIA_OK ||
        AImage_getPlaneData(image, 1, &u, &uLen) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 2, &vRow) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 2, &vPix) != AMEDIA_OK ||
        AImage_getPlaneData(image, 2, &v, &vLen) != AMEDIA_OK) {
        return false;
    }
    const size_t size = static_cast<size_t>(width) * height * 3 / 2;
    if (out->size() != size) {
        out->resize(size);
    }

    if (yPix == 1 && yRow == width && crop.left == 0) {
        std::memcpy(out->data(), y + static_cast<size_t>(crop.top) * yRow,
                    static_cast<size_t>(width) * height);
    } else {
        for (int32_t row = 0; row < height; ++row) {
            std::memcpy(out->data() + static_cast<size_t>(row) * width,
                        y + static_cast<size_t>(crop.top + row) * yRow +
                            static_cast<size_t>(crop.left) * yPix,
                        width);
        }
    }

    uint8_t* vu = out->data() + static_cast<size_t>(width) * height;
    if (uPix == 2 && vPix == 2 && u == v + 1) {
        for (int32_t row = 0; row < height / 2; ++row) {
            std::memcpy(vu + static_cast<size_t>(row) * width,
                        v + static_cast<size_t>(crop.top / 2 + row) * vRow +
                            static_cast<size_t>(crop.left / 2) * vPix,
                        width);
        }
    } else {
        for (int32_t row = 0; row < height / 2; ++row) {
            const uint8_t* uRowPtr = u + static_cast<size_t>(crop.top / 2 + row) * uRow +
                                     static_cast<size_t>(crop.left / 2) * uPix;
            const uint8_t* vRowPtr = v + static_cast<size_t>(crop.top / 2 + row) * vRow +
                                     static_cast<size_t>(crop.left / 2) * vPix;
            uint8_t* dst = vu + static_cast<size_t>(row) * width;
            for (int32_t col = 0; col < width / 2; ++col) {
                dst[col * 2] = vRowPtr[static_cast<size_t>(col) * vPix];
                dst[col * 2 + 1] = uRowPtr[static_cast<size_t>(col) * uPix];
            }
        }
    }
    return true;
}

}  // namespace

CameraSource::~CameraSource() {
    stop();
}

void CameraSource::onDeviceDisconnected(void* ctx, ACameraDevice*) {
    auto* self = static_cast<CameraSource*>(ctx);
    LOG(WARNING) << "camera disconnected";
    self->mRunning = false;
}

void CameraSource::onDeviceError(void*, ACameraDevice*, int error) {
    LOG(ERROR) << "camera error=" << error;
}

void CameraSource::onSessionReady(void*, ACameraCaptureSession*) {}

void CameraSource::onSessionClosed(void*, ACameraCaptureSession*) {}

void CameraSource::onImage(void* ctx, AImageReader* reader) {
    static_cast<CameraSource*>(ctx)->handleImage(reader);
}

void CameraSource::onPreviewImage(void*, AImageReader* reader) {
    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK && image != nullptr) {
        AImage_delete(image);
    }
}

void CameraSource::renderPreview(const uint8_t* nv21, int32_t width, int32_t height) {
    if (mRenderWindow == nullptr) {
        return;
    }

    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(mRenderWindow, &buffer, nullptr) != 0) {
        return;
    }
    if (buffer.bits == nullptr) {
        ANativeWindow_unlockAndPost(mRenderWindow);
        return;
    }

    const int rotation = (360 - mSensorOrientation % 360) % 360;
    const int rotatedWidth = rotation % 180 ? height : width;
    const int rotatedHeight = rotation % 180 ? width : height;
    const int offsetX = (rotatedWidth - kPreviewSize) / 2;
    const int offsetY = (rotatedHeight - kPreviewSize) / 2;
    const uint8_t* vu = nv21 + width * height;

    for (int v = 0; v < kPreviewSize; ++v) {
        auto* row = static_cast<uint32_t*>(buffer.bits) + v * buffer.stride;
        for (int u = 0; u < kPreviewSize; ++u) {
            const int rx = u + offsetX;
            const int ry = v + offsetY;
            int mx = 0;
            int my = 0;
            switch (rotation) {
                case 90:
                    mx = ry;
                    my = height - 1 - rx;
                    break;
                case 180:
                    mx = width - 1 - rx;
                    my = height - 1 - ry;
                    break;
                case 270:
                    mx = width - 1 - ry;
                    my = rx;
                    break;
                default:
                    mx = rx;
                    my = ry;
                    break;
            }
            const int x = width - 1 - mx;
            const int y = my;

            const uint8_t* c = vu + (y / 2) * width + (x & ~1);
            const int cb = c[1] - 128;
            const int cr = c[0] - 128;
            const int luma = nv21[y * width + x];
            const auto clamp8 = [](int value) {
                return static_cast<uint8_t>(std::clamp(value, 0, 255));
            };
            row[u] =
                0xff000000u | (static_cast<uint32_t>(clamp8(luma + ((116130 * cb) >> 16))) << 16) |
                (static_cast<uint32_t>(clamp8(luma - ((22554 * cb + 46802 * cr) >> 16))) << 8) |
                static_cast<uint32_t>(clamp8(luma + ((91881 * cr) >> 16)));
        }
    }
    ANativeWindow_unlockAndPost(mRenderWindow);
}

void CameraSource::handleImage(AImageReader* reader) {
    if (!mRunning.load()) {
        return;
    }
    AImage* image = nullptr;
    if (AImageReader_acquireNextImage(reader, &image) != AMEDIA_OK || image == nullptr) {
        return;
    }
    int32_t width = 0;
    int32_t height = 0;
    if (nv21FromImage(image, &mNv21, &width, &height)) {
        renderPreview(mNv21.data(), width, height);
        if (mCallback) {
            mCallback(mNv21.data(), width, height, mOrientation);
        }
    }
    AImage_delete(image);
}

bool CameraSource::start(ANativeWindow* preview, FrameCallback cb) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mRunning.load()) {
        return true;
    }
    mCallback = std::move(cb);
    mRenderWindow = nullptr;

    auto* manager = ACameraManager_create();
    mManager = manager;
    if (manager == nullptr) {
        return false;
    }

    int sensor = 0;
    const std::string id = chooseFrontCamera(manager, &sensor);
    if (id.empty()) {
        LOG(ERROR) << "no front camera";
        closeLocked();
        return false;
    }
    mSensorOrientation = sensor;
    mOrientation = orientationFromSensor(sensor);

    auto* reader = static_cast<AImageReader*>(nullptr);
    if (AImageReader_newWithUsage(kWidth, kHeight, AIMAGE_FORMAT_YUV_420_888,
                                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 3, &reader) != AMEDIA_OK ||
        reader == nullptr) {
        LOG(ERROR) << "analysis reader failed";
        closeLocked();
        return false;
    }
    mReader = reader;
    ANativeWindow* analysisWindow = nullptr;
    if (AImageReader_getWindow(reader, &analysisWindow) != AMEDIA_OK || !analysisWindow) {
        closeLocked();
        return false;
    }
    AImageReader_ImageListener listener{};
    listener.context = this;
    listener.onImageAvailable = onImage;
    AImageReader_setImageListener(reader, &listener);

    AImageReader* previewReader = nullptr;
    ANativeWindow* previewWindow = nullptr;
    if (AImageReader_newWithUsage(kWidth, kHeight, AIMAGE_FORMAT_PRIVATE,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4,
                                  &previewReader) == AMEDIA_OK &&
        previewReader != nullptr &&
        AImageReader_getWindow(previewReader, &previewWindow) == AMEDIA_OK &&
        previewWindow != nullptr) {
        AImageReader_ImageListener previewListener{};
        previewListener.context = this;
        previewListener.onImageAvailable = onPreviewImage;
        AImageReader_setImageListener(previewReader, &previewListener);
        mPreviewReader = previewReader;
    } else {
        if (previewReader != nullptr) {
            AImageReader_delete(previewReader);
        }
        previewWindow = nullptr;
    }

    if (preview != nullptr) {
        ANativeWindow_acquire(preview);
        mRenderWindow = preview;
        ANativeWindow_setBuffersGeometry(preview, kPreviewSize, kPreviewSize,
                                         AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        LOG(INFO) << "software preview target " << kPreviewSize;
    }

    ACameraDevice_StateCallbacks deviceCb{};
    deviceCb.context = this;
    deviceCb.onDisconnected = onDeviceDisconnected;
    deviceCb.onError = onDeviceError;
    ACameraDevice* device = nullptr;
    if (ACameraManager_openCamera(manager, id.c_str(), &deviceCb, &device) != ACAMERA_OK ||
        device == nullptr) {
        LOG(ERROR) << "openCamera failed";
        closeLocked();
        return false;
    }
    mDevice = device;

    ACaptureSessionOutputContainer* outputs = nullptr;
    ACaptureSessionOutputContainer_create(&outputs);
    mOutputs = outputs;
    ACaptureSessionOutput* analysisOutput = nullptr;
    ACaptureSessionOutput_create(analysisWindow, &analysisOutput);
    mOutput = analysisOutput;
    ACaptureSessionOutputContainer_add(outputs, analysisOutput);
    if (previewWindow != nullptr) {
        ACaptureSessionOutput* previewOutput = nullptr;
        ACaptureSessionOutput_create(previewWindow, &previewOutput);
        mPreviewOutput = previewOutput;
        ACaptureSessionOutputContainer_add(outputs, previewOutput);
    }

    ACameraCaptureSession_stateCallbacks sessionCb{};
    sessionCb.context = this;
    sessionCb.onClosed = onSessionClosed;
    sessionCb.onReady = onSessionReady;
    ACameraCaptureSession* session = nullptr;
    if (ACameraDevice_createCaptureSession(device, outputs, &sessionCb, &session) != ACAMERA_OK ||
        session == nullptr) {
        LOG(ERROR) << "createCaptureSession failed";
        closeLocked();
        return false;
    }
    mSession = session;

    ACaptureRequest* request = nullptr;
    if (ACameraDevice_createCaptureRequest(device, TEMPLATE_PREVIEW, &request) != ACAMERA_OK ||
        request == nullptr) {
        closeLocked();
        return false;
    }
    mRequest = request;
    ACameraOutputTarget* target = nullptr;
    ACameraOutputTarget_create(analysisWindow, &target);
    mTarget = target;
    ACaptureRequest_addTarget(request, target);
    if (previewWindow != nullptr) {
        ACameraOutputTarget* previewTarget = nullptr;
        ACameraOutputTarget_create(previewWindow, &previewTarget);
        mPreviewTarget = previewTarget;
        ACaptureRequest_addTarget(request, previewTarget);
    }

    uint8_t afMode = ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_AF_MODE, 1, &afMode);
    uint8_t sceneMode = ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY;
    ACaptureRequest_setEntry_u8(request, ACAMERA_CONTROL_SCENE_MODE, 1, &sceneMode);
    uint8_t faceDetect = ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE;
    ACaptureRequest_setEntry_u8(request, ACAMERA_STATISTICS_FACE_DETECT_MODE, 1, &faceDetect);

    mRunning = true;
    if (ACameraCaptureSession_setRepeatingRequest(session, nullptr, 1, &request, nullptr) !=
        ACAMERA_OK) {
        LOG(ERROR) << "setRepeatingRequest failed";
        mRunning = false;
        closeLocked();
        return false;
    }
    LOG(INFO) << "camera started id=" << id << " orientation=" << static_cast<int>(mOrientation)
              << " preview=" << (preview != nullptr);
    return true;
}

void CameraSource::closeLocked() {
    if (mRunning.load() || mDevice != nullptr || mSession != nullptr) {
        LOG(INFO) << "camera stopping";
    }
    mRunning = false;
    if (mReader != nullptr) {
        auto* reader = static_cast<AImageReader*>(mReader);
        AImageReader_setImageListener(reader, nullptr);
    }
    if (mSession != nullptr) {
        auto* session = static_cast<ACameraCaptureSession*>(mSession);
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
        mSession = nullptr;
    }
    if (mDevice != nullptr) {
        ACameraDevice_close(static_cast<ACameraDevice*>(mDevice));
        mDevice = nullptr;
    }
    if (mReader != nullptr) {
        AImageReader_delete(static_cast<AImageReader*>(mReader));
        mReader = nullptr;
    }
    if (mPreviewReader != nullptr) {
        AImageReader_delete(static_cast<AImageReader*>(mPreviewReader));
        mPreviewReader = nullptr;
    }
    if (mManager != nullptr) {
        ACameraManager_delete(static_cast<ACameraManager*>(mManager));
        mManager = nullptr;
    }
    if (mRequest != nullptr) {
        ACaptureRequest_free(static_cast<ACaptureRequest*>(mRequest));
        mRequest = nullptr;
    }
    if (mPreviewTarget != nullptr) {
        ACameraOutputTarget_free(static_cast<ACameraOutputTarget*>(mPreviewTarget));
        mPreviewTarget = nullptr;
    }
    if (mTarget != nullptr) {
        ACameraOutputTarget_free(static_cast<ACameraOutputTarget*>(mTarget));
        mTarget = nullptr;
    }
    if (mOutputs != nullptr) {
        ACaptureSessionOutputContainer_free(static_cast<ACaptureSessionOutputContainer*>(mOutputs));
        mOutputs = nullptr;
    }
    if (mPreviewOutput != nullptr) {
        ACaptureSessionOutput_free(static_cast<ACaptureSessionOutput*>(mPreviewOutput));
        mPreviewOutput = nullptr;
    }
    if (mOutput != nullptr) {
        ACaptureSessionOutput_free(static_cast<ACaptureSessionOutput*>(mOutput));
        mOutput = nullptr;
    }
    if (mRenderWindow != nullptr) {
        ANativeWindow_release(mRenderWindow);
        mRenderWindow = nullptr;
    }
    mCallback = nullptr;
}

void CameraSource::stop() {
    std::lock_guard<std::mutex> lock(mLock);
    closeLocked();
}

}  // namespace aidl::android::hardware::biometrics::face
