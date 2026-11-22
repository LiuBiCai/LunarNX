#pragma once

#ifdef _WIN32

#include "../input/gamepad_reader.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace lunar::desktop {

class DesktopAutomationServer {
public:
    using ScreenshotCallback =
        std::function<bool(std::vector<uint8_t>&, int&, int&)>;

    DesktopAutomationServer() = default;
    ~DesktopAutomationServer();
    DesktopAutomationServer(const DesktopAutomationServer&) = delete;
    DesktopAutomationServer& operator=(const DesktopAutomationServer&) = delete;

    bool start(const std::string& pipe_name,
               std::function<void(const input::GamepadState&)> input_callback);
    void pumpScreenshots(const ScreenshotCallback& capture);
    void stop();

private:
    struct ScreenshotRequest {
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        bool ok = false;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
    };

    void run();
    void handleClient(HANDLE pipe);
    bool handleMessage(HANDLE pipe, const std::string& message);
    void completeScreenshots();

    std::string pipe_name_;
    std::function<void(const input::GamepadState&)> input_callback_;
    std::atomic<bool> stopping_{false};
    std::atomic<HANDLE> active_pipe_{INVALID_HANDLE_VALUE};
    std::thread worker_;
    std::mutex screenshot_mutex_;
    std::deque<std::shared_ptr<ScreenshotRequest>> screenshot_requests_;
};

} // namespace lunar::desktop

#endif
