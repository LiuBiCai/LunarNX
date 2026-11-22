#define SDL_MAIN_HANDLED
#include "../src/platform/desktop_automation.h"
#include "../src/stream/video_renderer.h"
#include <SDL2/SDL.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    SDL_SetMainReady();
    lunar::stream::VideoRenderer renderer;
    lunar::input::GamepadReader gamepad;
    if (!renderer.initialize("LunarNX automation test", 320, 240) ||
        !gamepad.initialize()) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 2;
    }

    lunar::input::GamepadState scripted_state;
    scripted_state.a = true;
    scripted_state.left_stick_x = 1234;
    gamepad.setDesktopAutomationEnabled(true);
    gamepad.setDesktopAutomationState(scripted_state);
    gamepad.updateDesktopKeyboard();
    auto sampled_state = gamepad.read();
    if (!sampled_state.a || sampled_state.left_stick_x != 1234) return 9;

    gamepad.setDesktopAutomationMode(false);
    gamepad.setDesktopAutomationState(scripted_state);
    gamepad.updateDesktopKeyboard();
    sampled_state = gamepad.read();
    if (sampled_state.a || sampled_state.left_stick_x != 0) return 10;

    gamepad.setDesktopAutomationMode(true);
    gamepad.updateDesktopKeyboard();
    sampled_state = gamepad.read();
    if (sampled_state.a || sampled_state.left_stick_x != 0) return 11;

    AVFrame* frame = av_frame_alloc();
    if (!frame) return 3;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 320;
    frame->height = 240;
    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return 4;
    }
    for (int y = 0; y < frame->height; ++y) {
        std::memset(frame->data[0] + y * frame->linesize[0], 32, frame->width);
    }
    for (int y = 0; y < frame->height / 2; ++y) {
        std::memset(frame->data[1] + y * frame->linesize[1], 128, frame->width / 2);
        std::memset(frame->data[2] + y * frame->linesize[2], 128, frame->width / 2);
    }
    lunar::stream::VideoFrame video;
    video.avframe = frame;
    video.width = frame->width;
    video.height = frame->height;
    const bool rendered = renderer.render(video);
    av_frame_free(&frame);
    if (!rendered) return 5;
    renderer.present();

    gamepad.setDesktopAutomationEnabled(true);
    std::atomic<int> screenshot_count{0};
    std::atomic<bool> input_received{false};
    std::atomic<bool> release_received{false};
    lunar::desktop::DesktopAutomationServer server;
    if (!server.start(argv[1], [&gamepad, &input_received, &release_received](
                                   const lunar::input::GamepadState& state) {
            if (state.a || state.left_stick_x != 0 ||
                state.left_stick_y != 0 || state.left_trigger != 0) {
                input_received = true;
            } else if (input_received.load()) {
                release_received = true;
            }
            gamepad.setDesktopAutomationState(state);
        })) {
        return 6;
    }
    std::puts("READY");
    bool input_seen = false;
    bool release_seen = false;
    bool timeout_seen = false;
    std::chrono::steady_clock::time_point input_seen_at{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                server.stop();
                return 7;
            }
        }
        gamepad.updateDesktopKeyboard();
        const auto state = gamepad.read();
        if (state.a && state.left_stick_x == 1234 &&
            state.left_stick_y == -2345 && state.left_trigger == 54321) {
            if (!input_seen) input_seen_at = std::chrono::steady_clock::now();
            input_seen = true;
        }
        if (input_seen &&
            std::chrono::steady_clock::now() - input_seen_at >
                std::chrono::milliseconds(300) &&
            !state.a && state.left_stick_x == 0 && state.left_trigger == 0) {
            timeout_seen = true;
        }
        if (release_received.load() && input_seen && !state.a &&
            state.left_stick_x == 0 &&
            state.left_trigger == 0) {
            release_seen = true;
        }
        renderer.present();
        server.pumpScreenshots(
            [&renderer, &screenshot_count](std::vector<uint8_t>& pixels,
                                           int& width, int& height) {
                const bool ok = renderer.captureScreenshot(pixels, width, height);
                if (ok) screenshot_count.fetch_add(1);
                return ok;
            });
        if (input_seen && timeout_seen && release_seen &&
            screenshot_count.load() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    server.stop();
    gamepad.setDesktopAutomationEnabled(false);
    const bool screenshot_seen = screenshot_count.load() > 0;
    std::printf("RESULT input=%d timeout=%d release=%d screenshot=%d\n",
                input_seen, timeout_seen, release_seen, screenshot_seen);
    renderer.shutdown();
    return input_seen && timeout_seen && release_seen && screenshot_seen ? 0 : 8;
}
