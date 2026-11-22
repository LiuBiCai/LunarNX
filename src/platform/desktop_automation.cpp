#ifdef _WIN32

#include "desktop_automation.h"
#include <cJSON.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace lunar::desktop {
namespace {

constexpr DWORD kPipeBufferSize = 8192;
constexpr size_t kMaxRequestBytes = 8192;
constexpr auto kScreenshotTimeout = std::chrono::seconds(5);

bool readLine(HANDLE pipe, std::string& pending, std::string& line) {
    char buffer[512];
    while (pending.size() <= kMaxRequestBytes) {
        const size_t newline_pos = pending.find('\n');
        if (newline_pos != std::string::npos) {
            line.assign(pending, 0, newline_pos);
            pending.erase(0, newline_pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return line.size() <= kMaxRequestBytes;
        }
        DWORD bytes_read = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) ||
            bytes_read == 0) {
            return false;
        }
        pending.append(buffer, bytes_read);
    }
    return false;
}

bool writeBytes(HANDLE pipe, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(
            size > MAXDWORD ? MAXDWORD : size);
        DWORD written = 0;
        if (!WriteFile(pipe, bytes, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        bytes += written;
        size -= written;
    }
    return true;
}

bool writeLine(HANDLE pipe, const std::string& line) {
    return writeBytes(pipe, line.data(), line.size()) &&
           writeBytes(pipe, "\n", 1);
}

bool readBool(cJSON* object, const char* key, bool& value) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        value = false;
        return true;
    }
    if (!cJSON_IsBool(item)) return false;
    value = cJSON_IsTrue(item);
    return true;
}

bool readInteger(cJSON* object, const char* key, int minimum, int maximum,
                 int& value) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        value = 0;
        return true;
    }
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < minimum || item->valuedouble > maximum) {
        return false;
    }
    value = static_cast<int>(item->valuedouble);
    return true;
}

bool parseInputState(cJSON* root, input::GamepadState& state) {
    cJSON* object = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsObject(object)) return false;
    int left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    int left_trigger = 0, right_trigger = 0;
    const bool valid =
           readBool(object, "a", state.a) &&
           readBool(object, "b", state.b) &&
           readBool(object, "x", state.x) &&
           readBool(object, "y", state.y) &&
           readBool(object, "dpad_up", state.dpad_up) &&
           readBool(object, "dpad_down", state.dpad_down) &&
           readBool(object, "dpad_left", state.dpad_left) &&
           readBool(object, "dpad_right", state.dpad_right) &&
           readBool(object, "lb", state.lb) &&
           readBool(object, "rb", state.rb) &&
           readBool(object, "lt", state.lt) &&
           readBool(object, "rt", state.rt) &&
           readBool(object, "l3", state.l3) &&
           readBool(object, "r3", state.r3) &&
           readBool(object, "view", state.view) &&
           readBool(object, "menu", state.menu) &&
           readBool(object, "guide", state.guide) &&
           readBool(object, "touchpad", state.touchpad) &&
           readInteger(object, "left_stick_x", -32768, 32767, left_x) &&
           readInteger(object, "left_stick_y", -32768, 32767, left_y) &&
           readInteger(object, "right_stick_x", -32768, 32767, right_x) &&
           readInteger(object, "right_stick_y", -32768, 32767, right_y) &&
           readInteger(object, "left_trigger", 0, 65535, left_trigger) &&
           readInteger(object, "right_trigger", 0, 65535, right_trigger);
    if (!valid) return false;
    state.left_stick_x = static_cast<int16_t>(left_x);
    state.left_stick_y = static_cast<int16_t>(left_y);
    state.right_stick_x = static_cast<int16_t>(right_x);
    state.right_stick_y = static_cast<int16_t>(right_y);
    state.left_trigger = static_cast<uint16_t>(left_trigger);
    state.right_trigger = static_cast<uint16_t>(right_trigger);
    return true;
}

} // namespace

DesktopAutomationServer::~DesktopAutomationServer() {
    stop();
}

bool DesktopAutomationServer::start(
    const std::string& pipe_name,
    std::function<void(const input::GamepadState&)> input_callback) {
    if (worker_.joinable() || pipe_name.empty() || !input_callback) return false;
    pipe_name_ = pipe_name;
    input_callback_ = std::move(input_callback);
    stopping_ = false;
    try {
        worker_ = std::thread(&DesktopAutomationServer::run, this);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: Could not start automation pipe: %s\n", e.what());
        return false;
    }
    return true;
}

void DesktopAutomationServer::run() {
    while (!stopping_.load(std::memory_order_acquire)) {
        HANDLE pipe = CreateNamedPipeA(
            pipe_name_.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBufferSize, kPipeBufferSize, 0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "ERROR: Could not create automation pipe (Windows error %lu).\n",
                         GetLastError());
            stopping_ = true;
            break;
        }
        active_pipe_.store(pipe, std::memory_order_release);
        if (stopping_.load(std::memory_order_acquire)) {
            CloseHandle(pipe);
            active_pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
            break;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        if (connected || connect_error == ERROR_PIPE_CONNECTED) {
            handleClient(pipe);
        } else if (connect_error != ERROR_OPERATION_ABORTED &&
                   !stopping_.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "Automation pipe connection failed (Windows error %lu).\n",
                         connect_error);
        }
        DisconnectNamedPipe(pipe);
        active_pipe_.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        CloseHandle(pipe);
    }
    if (input_callback_) input_callback_(input::GamepadState{});
}

void DesktopAutomationServer::handleClient(HANDLE pipe) {
    std::string pending;
    while (!stopping_.load(std::memory_order_acquire)) {
        std::string message;
        if (!readLine(pipe, pending, message)) break;
        if (message.empty() || !handleMessage(pipe, message)) break;
    }
    if (input_callback_) input_callback_(input::GamepadState{});
}

bool DesktopAutomationServer::handleMessage(
    HANDLE pipe, const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.c_str(), message.size());
    if (!root) return writeLine(pipe, R"({"ok":false,"error":"invalid_json"})");
    cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return writeLine(pipe, R"({"ok":false,"error":"missing_type"})");
    }

    if (std::strcmp(type->valuestring, "input") == 0) {
        input::GamepadState state;
        if (!parseInputState(root, state)) {
            cJSON_Delete(root);
            return writeLine(pipe, R"({"ok":false,"error":"invalid_state"})");
        }
        cJSON_Delete(root);
        input_callback_(state);
        return writeLine(pipe, R"({"ok":true})");
    }

    if (std::strcmp(type->valuestring, "release") == 0) {
        cJSON_Delete(root);
        input_callback_(input::GamepadState{});
        return writeLine(pipe, R"({"ok":true})");
    }

    if (std::strcmp(type->valuestring, "screenshot") == 0) {
        cJSON_Delete(root);
        auto request = std::make_shared<ScreenshotRequest>();
        {
            std::lock_guard<std::mutex> lock(screenshot_mutex_);
            screenshot_requests_.push_back(request);
        }
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        {
            std::unique_lock<std::mutex> lock(request->mutex);
            if (!request->ready.wait_for(lock, kScreenshotTimeout,
                                         [&] { return request->done; })) {
                return writeLine(pipe, R"({"ok":false,"error":"screenshot_timeout"})");
            }
            if (!request->ok) {
                return writeLine(pipe, R"({"ok":false,"error":"screenshot_unavailable"})");
            }
            pixels = std::move(request->pixels);
            width = request->width;
            height = request->height;
        }
        const size_t byte_count = pixels.size();
        const std::string header =
            "{\"ok\":true,\"format\":\"RGB24\",\"width\":" +
            std::to_string(width) + ",\"height\":" + std::to_string(height) +
            ",\"bytes\":" + std::to_string(byte_count) + "}";
        return writeLine(pipe, header) &&
               writeBytes(pipe, pixels.data(), pixels.size());
    }

    cJSON_Delete(root);
    return writeLine(pipe, R"({"ok":false,"error":"unknown_type"})");
}

void DesktopAutomationServer::pumpScreenshots(
    const ScreenshotCallback& capture) {
    std::deque<std::shared_ptr<ScreenshotRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        requests.swap(screenshot_requests_);
    }
    for (const auto& request : requests) {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        const bool ok = capture && capture(pixels, width, height);
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->ok = ok;
            request->width = width;
            request->height = height;
            request->pixels = std::move(pixels);
            request->done = true;
        }
        request->ready.notify_one();
    }
}

void DesktopAutomationServer::completeScreenshots() {
    std::deque<std::shared_ptr<ScreenshotRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(screenshot_mutex_);
        requests.swap(screenshot_requests_);
    }
    for (const auto& request : requests) {
        {
            std::lock_guard<std::mutex> lock(request->mutex);
            request->done = true;
            request->ok = false;
        }
        request->ready.notify_one();
    }
}

void DesktopAutomationServer::stop() {
    if (!worker_.joinable()) return;
    stopping_.store(true, std::memory_order_release);
    completeScreenshots();
    CancelSynchronousIo(worker_.native_handle());
    worker_.join();
}

} // namespace lunar::desktop

#endif
