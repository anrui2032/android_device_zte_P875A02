/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "SubHal.h"

#include "UdfpsUeventMonitor.h"

#include <mutex>
#include <string>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

using ::android::hardware::sensors::V1_0::Event;
using ::android::hardware::sensors::V1_0::OperationMode;
using ::android::hardware::sensors::V1_0::RateLevel;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorInfo;
using ::android::hardware::sensors::V1_0::SharedMemInfo;

/**
 * Sensors HAL sub-HAL that exposes the devices UDFPS "long press" sensor.
 *
 * The device ships a prebuilt Qualcomm SSC sensors HAL (sensors.ssc.so) that is
 * loaded by android.hardware.sensors-service.multihal. Instead of replacing it,
 * this library is loaded alongside it: the multihal discovers sub-HALs through
 * /vendor/etc/sensors/hals.conf and merges their sensor lists, prefixing every
 * handle with the sub-HAL index it was loaded from.
 *
 * Exactly one sensor is exposed:
 *
 *   typeAsString : org.lineageos.sensor.udfps
 *   type         : SENSOR_TYPE_DEVICE_PRIVATE_BASE + 3
 *   reporting    : one-shot, wake-up
 *
 * SystemUI looks that string type up through
 * config_dozeUdfpsLongPressSensorType (DozeSensors#findSensor), registers it
 * with SensorManager#requestTriggerSensor and forwards the resulting event to
 * AuthController#onAodInterrupt, which finally reaches
 * UdfpsController#onAodInterrupt and starts fingerprint authentication.
 *
 * The event source is the kernel uevent emitted by the ZTE Goodix touch driver
 * when a finger presses the FOD area while the panel is in AOD. See
 * UdfpsUeventMonitor for the details and the supporting evidence.
 */
class UdfpsSubHal : public ISensorsSubHal {
  public:
    UdfpsSubHal() = default;

    // Methods from
    // ::android::hardware::sensors::V2_0::implementation::ISensorsSubHal.
    Return<Result> initialize(const sp<IHalProxyCallback>& halProxyCallback) override;
    Return<void> debug(const hidl_handle& fd, const hidl_vec<hidl_string>& args) override;
    const std::string getName() override { return "UdfpsSubHal"; }

    // Methods from ::android::hardware::sensors::V2_0::ISensors.
    Return<void> getSensorsList(V2_0::ISensors::getSensorsList_cb _hidl_cb) override;
    Return<Result> setOperationMode(OperationMode mode) override;
    Return<Result> activate(int32_t sensorHandle, bool enabled) override;
    Return<Result> batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                         int64_t maxReportLatencyNs) override;
    Return<Result> flush(int32_t sensorHandle) override;
    Return<Result> injectSensorData(const Event& event) override;
    Return<void> registerDirectChannel(
            const SharedMemInfo& mem,
            V2_0::ISensors::registerDirectChannel_cb _hidl_cb) override;
    Return<Result> unregisterDirectChannel(int32_t channelHandle) override;
    Return<void> configDirectReport(int32_t sensorHandle, int32_t channelHandle, RateLevel rate,
                                    V2_0::ISensors::configDirectReport_cb _hidl_cb) override;

  private:
    void postTriggerEvent();

    std::mutex mLock;
    sp<IHalProxyCallback> mCallback;
    OperationMode mOperationMode = OperationMode::NORMAL;

    /*
     * A one-shot sensor is armed by activate(sensorHandle, true) and fires at
     * most once per arming, so the trigger is latched here and only cleared by
     * the next activation. See activate().
     */
    bool mActive = false;
    bool mTriggered = false;

    UdfpsUeventMonitor mMonitor;
};

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android
