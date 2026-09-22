/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

/*
 * Emitted by the ZTE Goodix touch driver when a finger lands inside the FOD
 * area while the panel is in AOD/doze. The screen-on variant of the same event
 * is "areameet_down=true"; it is deliberately ignored here because the normal
 * touch path already drives the UDFPS overlay while the screen is on.
 *
 * The driver emits five related tokens in total (tpd_ufp_mac.h):
 *
 *   aod_areameet_down=true   AOD press      - the only one matched here
 *   areameet_down=true       screen-on press
 *   areameet_up=true         finger lifted  - the AOD lift reuses this token
 *   single_tap=true          single tap gesture, also sent just before the AOD
 *                            down event, so one press produces two uevents
 *   double_tap=true          double tap gesture
 *
 * Matching single_tap would make an ordinary tap trigger authentication, and
 * matching areameet_down would duplicate what the lit-screen touch path
 * already does.
 */
constexpr char kAodAreaMeetDownToken[] = "aod_areameet_down=true";

/* DEVPATH of the uevent carrier device created by ufp_mac_init(). */
constexpr char kZteTouchDevPathToken[] = "DEVPATH=/devices/platform/zte_touch";

/**
 * Watches the kernel kobject uevent stream for the ZTE touch driver's finger
 * area meet notification and reports the AOD variant to a callback.
 *
 * Mechanism (verified against anrui2032/android_kernel_zte_sm8350, branch
 * lineage-23.2, drivers/input/touchscreen/goodix_berlin_driver_zte):
 *
 *   gsx_gesture_ist()                                   (goodix_ts_gesture.c)
 *     -> report_ufp_uevent(UFP_FP_DOWN)                 (tpd_ufp_mac.c)
 *       -> __report_ufp_uevent(AOD_AREAMEET_DOWN)
 *         -> kobject_uevent_env(&zte_touch->dev.kobj, KOBJ_CHANGE,
 *                               {"aod_areameet_down=true",
 *                                "TP_POWER_STATUS=3"})
 *
 * The "zte_touch" platform device that carries these uevents is created in
 * ufp_mac_init(). It has no sysfs attribute, so the uevent stream is the only
 * userspace interface for this event.
 *
 * The netlink socket is opened in start() and closed in stop(), so the process
 * only subscribes to the (system wide) uevent stream while the sensor is
 * activated by the framework, that is, while the device is dozing.
 */
class UdfpsUeventMonitor {
  public:
    using Callback = std::function<void()>;

    UdfpsUeventMonitor() = default;
    ~UdfpsUeventMonitor();

    UdfpsUeventMonitor(const UdfpsUeventMonitor&) = delete;
    UdfpsUeventMonitor& operator=(const UdfpsUeventMonitor&) = delete;

    /** Opens the netlink socket and starts the reader thread. */
    bool start(Callback callback);

    /** Stops the reader thread and closes the netlink socket. */
    void stop();

    bool running() const { return mRunning.load(); }

  private:
    void readLoop();

    Callback mCallback;
    std::thread mThread;
    std::atomic<bool> mRunning{false};
    int mSocketFd = -1;
};

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android
