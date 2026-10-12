/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "eventloop.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static const size_t kControlRead = 0;
static const size_t kControlWrite = 1;

static void closeIfOpen(int* fd) {
    if (*fd != -1) {
        ::close(*fd);
        *fd = -1;
    }
}

EventLoop::EventLoop() {
    mControlPipe[kControlRead] = -1;
    mControlPipe[kControlWrite] = -1;
}

EventLoop::~EventLoop() {
    closeIfOpen(&mControlPipe[kControlRead]);
    closeIfOpen(&mControlPipe[kControlWrite]);
}

bool EventLoop::init() {
    if (mControlPipe[kControlRead] != -1) {
        ALOGE("Event loop already initialized");
        return false;
    }

    int status = ::pipe2(mControlPipe, O_CLOEXEC);
    if (status != 0) {
        ALOGE("Failed to create control pipe: %s", strerror(errno));
        return false;
    }

    return true;
}

void EventLoop::stop(StopHandler handler) {
    char stop = 1;
    // Set the handler before writing so that it's guaranteed to be available
    // when the event loop reads from the control pipe.
    {
        // No need to keep the lock while writing so make it scoped
        std::unique_lock<std::mutex> lock(mStopHandlerMutex);
        mStopHandler = handler;
    }
    ssize_t bytesWritten;
    do {
        bytesWritten = ::write(mControlPipe[kControlWrite], &stop, sizeof(stop));
    } while (bytesWritten < 0 && errno == EINTR);
    if (bytesWritten != sizeof(stop)) {
        ALOGE("Failed to write to control pipe: %s", strerror(errno));
    }
}

bool EventLoop::eventLoop() {
    struct pollfd fd;
    memset(&fd, 0, sizeof(fd));
    fd.fd = mControlPipe[kControlRead];
    fd.events = POLLIN;

    for (;;) {
        int status = ::poll(&fd, 1, -1);
        if (status == 0) {
            // Timeout, not really supposed to happen
            ALOGW("poll encountered a timeout despite infinite timeout");
            continue;
        } else if (status < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGE("poll encountered an error: %s", strerror(errno));
            return false;
        }
        if ((fd.revents & POLLIN) == 0) {
            if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ALOGE("control pipe failed, revents 0x%x", fd.revents);
                return false;
            }
            continue;
        }
        if (readControlMessage()) {
            // Make a copy of the stop handler while holding the lock and then
            // call it after releasing the lock. This prevents the potential
            // deadlock of someone calling stop from the stop callback.
            StopHandler handler;
            {
                std::unique_lock<std::mutex> lock(mStopHandlerMutex);
                handler = mStopHandler;
            }
            if (handler) {
                handler();
            }
            return true;
        }
    }
}

bool EventLoop::readControlMessage() {
    char buffer[32];

    for (;;) {
        // goldfish used recv() here, which fails with ENOTSOCK on a pipe.
        ssize_t bytesReceived = ::read(mControlPipe[kControlRead],
                                       buffer,
                                       sizeof(buffer));
        if (bytesReceived < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGE("Failed to read control pipe: %s", strerror(errno));
            return false;
        }
        return bytesReceived > 0;
    }
}
