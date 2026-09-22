/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "UdfpsSubHal.h"

#include <android-base/logging.h>

#include <unistd.h>

#include <utils/SystemClock.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

using ::android::hardware::Void;
using ::android::hardware::sensors::V1_0::Event;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorInfo;

using ::android::hardware::sensors::V2_0::implementation::UdfpsSubHal;

namespace {

/*
 * Reported through SensorInfo#typeAsString. It must match
 * config_dozeUdfpsLongPressSensorType in the device overlay exactly, otherwise
 * SystemUI will not find the sensor.
 */
constexpr char kUdfpsSensorStringType[] = "org.lineageos.sensor.udfps";

/*
 * The HalProxy stores the index of the sub-HAL it loaded a sensor from in the
 * upper byte of the handle it hands to the framework, so a sub-HAL only owns
 * the lower three bytes.
 */
constexpr int32_t kUdfpsSensorHandle = 0;

/*
 * Numeric sensor type, spelled out instead of using the generated enum because
 * the value is part of the HAL contract:
 *
 *   SENSOR_TYPE_DEVICE_PRIVATE_BASE (0x10000) + 3
 *
 * android.hardware.sensors@1.0::types reserves everything at or above
 * DEVICE_PRIVATE_BASE for OEM/custom sensors and requires them to carry their
 * payload in Event::u.data ("The following sensors should use the data field").
 * The HalProxy follows the same rule: ConvertUtils::convertToAidlEvent handles
 * every type >= DEVICE_PRIVATE_BASE in its default branch by copying all 16
 * floats of u.data into the payload the framework sees.
 */
constexpr int32_t kUdfpsSensorType = 0x10000 /* DEVICE_PRIVATE_BASE */ + 3;

/*
 *   0x1 : SensorFlagBits::WAKE_UP
 *   0x4 : SensorFlagBits::ONE_SHOT_MODE
 *
 * A one-shot sensor is edge triggered, so it must not advertise a sampling
 * mode; wake-up is required because the event has to wake the AP from AOD.
 */
constexpr uint32_t kUdfpsSensorFlags = 0x1u | 0x4u;

/*
 * FOD geometry in display pixels; the panel is 1080x2400. These values must
 * stay in sync with config_udfps_sensor_props in the device overlay, which
 * resolves to 540 / 2068 / 94 on this device.
 *
 * The kernel reports neither a position nor a size for the AOD area meet
 * event, so the configured sensor centre is reported as the touch position and
 * the sensor radius is used for both touch axes. DozeSensors forwards
 * values[0]/values[1] to DozeTriggers as absolute screen coordinates, and
 * DozeTriggers reads values[3]/values[4] as the major/minor touch axes.
 */
constexpr float kFodCenterX = 540.0f;
constexpr float kFodCenterY = 2068.0f;
constexpr float kFodRadius = 94.0f;

/*
 * The payload carries absolute display pixel coordinates, so the largest value
 * the sensor can report is the panel's long edge.
 */
constexpr float kDisplayMaxDimension = 2400.0f;

}  // namespace

/*
 * Entry point the HalProxy looks up with dlsym() after dlopen()ing this
 * library. The version must be SUB_HAL_2_0_VERSION or the HalProxy refuses to
 * register the sub-HAL.
 */
extern "C" ISensorsSubHal* sensorsHalGetSubHal(uint32_t* version) {
    static UdfpsSubHal subHal;
    *version = SUB_HAL_2_0_VERSION;
    return &subHal;
}

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

Return<Result> UdfpsSubHal::initialize(const sp<IHalProxyCallback>& halProxyCallback) {
    /*
     * initialize() runs again every time the sensors framework restarts, so the
     * sub-HAL has to return to a clean state.
     */
    mMonitor.stop();

    std::lock_guard<std::mutex> lock(mLock);
    mCallback = halProxyCallback;
    mOperationMode = OperationMode::NORMAL;
    mActive = false;
    mTriggered = false;

    return Result::OK;
}

Return<void> UdfpsSubHal::getSensorsList(V2_0::ISensors::getSensorsList_cb _hidl_cb) {
    SensorInfo sensor = {};

    sensor.sensorHandle = kUdfpsSensorHandle;
    sensor.name = "UDFPS Long Press Sensor";
    sensor.vendor = "ZTE";
    sensor.version = 1;
    sensor.type = static_cast<V1_0::SensorType>(kUdfpsSensorType);
    sensor.typeAsString = kUdfpsSensorStringType;
    sensor.maxRange = kDisplayMaxDimension;
    sensor.resolution = 1.0f;
    sensor.power = 0.001f;
    /* One-shot sensors report a minimum delay of -1 and no batching. */
    sensor.minDelay = -1;
    sensor.maxDelay = 0;
    sensor.fifoReservedEventCount = 0;
    sensor.fifoMaxEventCount = 0;
    sensor.requiredPermission = "";
    sensor.flags = kUdfpsSensorFlags;

    hidl_vec<SensorInfo> list;
    list.resize(1);
    list[0] = sensor;

    _hidl_cb(list);
    return Void();
}

Return<Result> UdfpsSubHal::setOperationMode(OperationMode mode) {
    /*
     * The HalProxy forwards this call to every sub-HAL and fails the whole
     * operation if one of them rejects it, so rejecting DATA_INJECTION here
     * would take the SSC sub-HAL down with us. The sensors reference sub-HAL
     * likewise stores whatever mode it is handed and only uses it to suppress
     * events; injection itself is reported as unsupported by
     * injectSensorData().
     */
    std::lock_guard<std::mutex> lock(mLock);
    mOperationMode = mode;

    return Result::OK;
}

Return<Result> UdfpsSubHal::activate(int32_t sensorHandle, bool enabled) {
    if (sensorHandle != kUdfpsSensorHandle) {
        return Result::BAD_VALUE;
    }

    /*
     * Re-arm the one-shot latch. SystemUI's DozeSensors TriggerSensor releases
     * the sensor after every trigger and requests it again on the next AOD
     * state change, so each press starts with a fresh activate(true) and this
     * reset is what makes the second (and every later) screen-off press work.
     */
    {
        std::lock_guard<std::mutex> lock(mLock);
        mActive = enabled;
        mTriggered = false;
    }

    if (enabled) {
        if (!mMonitor.start([this] { postTriggerEvent(); })) {
            /*
             * start() failed to allocate or bind the netlink socket, so the
             * sensor cannot report anything and must not be reported as
             * enabled.
             */
            LOG(ERROR) << "Failed to start monitoring the FOD uevent stream";
            std::lock_guard<std::mutex> lock(mLock);
            mActive = false;
            return Result::NO_MEMORY;
        }
    } else {
        mMonitor.stop();
    }

    return Result::OK;
}

Return<Result> UdfpsSubHal::batch(int32_t sensorHandle, int64_t /* samplingPeriodNs */,
                                  int64_t /* maxReportLatencyNs */) {
    /* One-shot sensors do not batch; the sampling parameters are ignored. */
    if (sensorHandle != kUdfpsSensorHandle) {
        return Result::BAD_VALUE;
    }

    return Result::OK;
}

Return<Result> UdfpsSubHal::flush(int32_t sensorHandle) {
    if (sensorHandle != kUdfpsSensorHandle) {
        return Result::BAD_VALUE;
    }

    /*
     * The sensor has no hardware FIFO, so there is nothing to flush. The HAL
     * specification also requires flush() on a one-shot sensor to be rejected
     * instead of generating a flush complete event.
     */
    return Result::BAD_VALUE;
}

Return<Result> UdfpsSubHal::injectSensorData(const Event& /* event */) {
    return Result::INVALID_OPERATION;
}

Return<void> UdfpsSubHal::registerDirectChannel(
        const SharedMemInfo& /* mem */, V2_0::ISensors::registerDirectChannel_cb _hidl_cb) {
    _hidl_cb(Result::INVALID_OPERATION, -1 /* channelHandle */);
    return Void();
}

Return<Result> UdfpsSubHal::unregisterDirectChannel(int32_t /* channelHandle */) {
    return Result::INVALID_OPERATION;
}

Return<void> UdfpsSubHal::configDirectReport(
        int32_t /* sensorHandle */, int32_t /* channelHandle */, RateLevel /* rate */,
        V2_0::ISensors::configDirectReport_cb _hidl_cb) {
    _hidl_cb(Result::INVALID_OPERATION, 0 /* reportToken */);
    return Void();
}

Return<void> UdfpsSubHal::debug(const hidl_handle& fd, const hidl_vec<hidl_string>& /* args */) {
    if (fd.getNativeHandle() == nullptr || fd->numFds < 1) {
        LOG(ERROR) << "Missing fd for debug output";
        return Void();
    }

    FILE* out = fdopen(dup(fd->data[0]), "w");
    if (out == nullptr) {
        LOG(ERROR) << "Failed to open debug fd";
        return Void();
    }

    fprintf(out, "UdfpsSubHal\n");
    fprintf(out, "  typeAsString  : %s\n", kUdfpsSensorStringType);
    fprintf(out, "  sensorHandle  : %d\n", kUdfpsSensorHandle);
    fprintf(out, "  type          : %d\n", kUdfpsSensorType);
    fprintf(out, "  flags         : 0x%x\n", kUdfpsSensorFlags);
    fprintf(out, "  operationMode : %d\n", static_cast<int>(mOperationMode));
    fprintf(out, "  active        : %s\n", mActive ? "yes" : "no");
    fprintf(out, "  triggered     : %s\n", mTriggered ? "yes" : "no");
    fprintf(out, "  monitoring    : %s\n", mMonitor.running() ? "yes" : "no");
    fprintf(out, "  fod center    : %f,%f r=%f\n", kFodCenterX, kFodCenterY, kFodRadius);
    fprintf(out, "  uevent token  : %s\n", kAodAreaMeetDownToken);

    fclose(out);
    return Void();
}

void UdfpsSubHal::postTriggerEvent() {
    sp<IHalProxyCallback> callback;
    {
        std::lock_guard<std::mutex> lock(mLock);

        /*
         * The kernel driver already reports the press edge only once, but the
         * one-shot contract is that this sensor fires at most once per arming,
         * so a second uevent that races in before DozeSensors releases the
         * sensor is dropped here instead of reaching the HalProxy. mActive is
         * also checked because an event can still be in flight between
         * activate(false) and the moment the monitor thread stops.
         */
        if (!mActive || mTriggered || mOperationMode == OperationMode::DATA_INJECTION) {
            return;
        }

        callback = mCallback;
        if (callback == nullptr) {
            LOG(WARNING) << "Dropping UDFPS trigger, the HalProxy callback is not set";
            return;
        }

        mTriggered = true;
    }

    Event event;
    event.sensorHandle = kUdfpsSensorHandle;
    event.sensorType = static_cast<V1_0::SensorType>(kUdfpsSensorType);
    event.timestamp = ::android::elapsedRealtimeNano();

    /*
     * A HIDL union has no usable discriminator on the C++ side; for custom
     * sensor types the HalProxy picks the EventPayload::data member purely from
     * event.sensorType, so the union is zeroed and only the data array is
     * filled in. This mirrors what the default sensors HAL does in
     * Sensor::readEvents().
     */
    memset(&event.u, 0, sizeof(event.u));
    event.u.data[0] = kFodCenterX; /* values[0] - screen X */
    event.u.data[1] = kFodCenterY; /* values[1] - screen Y */
    event.u.data[2] = 0.0f;
    event.u.data[3] = kFodRadius; /* values[3] - major axis */
    event.u.data[4] = kFodRadius; /* values[4] - minor axis */

    /*
     * The sensor is a wake-up sensor, so the scoped wake lock has to be locked
     * before handing the event over; the HalProxy treats a wake-up event with
     * an unlocked wake lock as a fatal sub-HAL bug.
     */
    LOG(INFO) << "AOD FOD press, posting UDFPS trigger at " << kFodCenterX << "," << kFodCenterY;

    ScopedWakelock wakelock = callback->createScopedWakelock(true);
    callback->postEvents({event}, std::move(wakelock));
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android
