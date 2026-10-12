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

#pragma once

#include <functional>
#include <mutex>

// The thread body of wifi_event_loop. This is the goldfish Netlink class
// without its rtnetlink socket: this HAL does not consume asynchronous events
// and its vendor commands are synchronous (see nl80211.h), so the loop only
// has to block until stop() is called.
class EventLoop {
public:
    using StopHandler = std::function<void ()>;
    EventLoop();
    ~EventLoop();

    bool init();
    void stop(StopHandler stopHandler);

    bool eventLoop();

private:
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    bool readControlMessage();

    int mControlPipe[2];
    std::mutex mStopHandlerMutex;
    StopHandler mStopHandler;
};
