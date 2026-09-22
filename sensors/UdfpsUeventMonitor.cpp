/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "UdfpsUeventMonitor.h"

#include <android-base/logging.h>

#include <linux/netlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

namespace {

/* A kobject uevent is a small, NUL separated list of KEY=VALUE strings. */
constexpr size_t kUeventBufferSize = 2048;

/* Keeps stop() responsive without busy polling. */
constexpr int kPollTimeoutMs = 250;

/*
 * Walks the NUL separated token list of a kobject uevent and returns whether
 * any token is exactly equal to |token|. A plain strstr() cannot be used here
 * because the payload contains embedded NUL bytes.
 */
bool hasUeventToken(const char* buffer, size_t length, const char* token) {
    size_t offset = 0;

    while (offset < length) {
        const char* entry = buffer + offset;
        size_t entryLength = strnlen(entry, length - offset);

        if (entryLength > 0 && strcmp(entry, token) == 0) {
            return true;
        }

        offset += entryLength + 1;
    }

    return false;
}

}  // namespace

UdfpsUeventMonitor::~UdfpsUeventMonitor() {
    stop();
}

bool UdfpsUeventMonitor::start(Callback callback) {
    if (mRunning.load()) {
        return true;
    }

    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        LOG(ERROR) << "Failed to create uevent netlink socket: " << strerror(errno);
        return false;
    }

    struct sockaddr_nl addr = {};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = static_cast<__u32>(getpid());
    addr.nl_groups = 1; /* kernel uevent group */

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG(ERROR) << "Failed to bind uevent netlink socket: " << strerror(errno);
        close(fd);
        return false;
    }

    mSocketFd = fd;
    mCallback = std::move(callback);
    mRunning.store(true);

    mThread = std::thread(&UdfpsUeventMonitor::readLoop, this);
    return true;
}

void UdfpsUeventMonitor::stop() {
    if (mRunning.exchange(false) && mThread.joinable()) {
        /* The reader leaves its poll() within kPollTimeoutMs. */
        mThread.join();
    }

    if (mSocketFd >= 0) {
        close(mSocketFd);
        mSocketFd = -1;
    }

    mCallback = nullptr;
}

void UdfpsUeventMonitor::readLoop() {
    char buffer[kUeventBufferSize];

    while (mRunning.load()) {
        struct pollfd pfd = {};
        pfd.fd = mSocketFd;
        pfd.events = POLLIN;

        int rc = poll(&pfd, 1, kPollTimeoutMs);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "Failed to poll uevent netlink socket: " << strerror(errno);
            break;
        }
        if (rc == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        ssize_t length = recv(mSocketFd, buffer, sizeof(buffer), 0);
        if (length < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "Failed to receive uevent: " << strerror(errno);
            break;
        }
        if (length == 0) {
            continue;
        }

        const size_t payloadLength = static_cast<size_t>(length);

        if (!hasUeventToken(buffer, payloadLength, kZteTouchDevPathToken)) {
            continue;
        }
        if (!hasUeventToken(buffer, payloadLength, kAodAreaMeetDownToken)) {
            continue;
        }

        LOG(INFO) << "FOD area meet reported while in AOD";

        if (mCallback) {
            mCallback();
        }
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android
