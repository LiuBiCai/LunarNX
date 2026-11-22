#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#include "../src/input/gamepad_reader.h"
#include "../src/stream/video_renderer.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    SDL_SetMainReady();
    lunar::stream::VideoRenderer renderer;
    lunar::input::GamepadReader gamepad;
    if (!renderer.initialize("LunarNX SDL test", 320, 240) || !gamepad.initialize()) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_GetWindowFromID(1);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info)) {
        std::fprintf(stderr, "Cannot get SDL window handle: %s\n", SDL_GetError());
        return 1;
    }

    std::atomic<bool> worker_done{false};
    std::atomic<bool> worker_ok{true};
    std::thread worker([&] {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            worker_ok = false;
            worker_done = true;
            return;
        }
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = 320;
        frame->height = 240;
        if (av_frame_get_buffer(frame, 32) < 0) {
            worker_ok = false;
        } else {
            lunar::stream::VideoFrame video;
            video.avframe = frame;
            video.width = frame->width;
            video.height = frame->height;
            if (!renderer.render(video)) worker_ok = false;
        }
        av_frame_free(&frame);

        DWORD_PTR result = 0;
        if (!SendMessageTimeout(info.info.win.window, WM_LBUTTONDOWN, MK_LBUTTON,
                                MAKELPARAM(10, 10), SMTO_ABORTIFHUNG, 3000, &result)) {
            worker_ok = false;
        }
        PostMessage(info.info.win.window, WM_KEYDOWN, '3',
                    1 | (MapVirtualKey('3', MAPVK_VK_TO_VSC) << 16));
        worker_done = true;
    });

    bool mouse_seen = false;
    bool trigger_seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_MOUSEBUTTONDOWN) mouse_seen = true;
        }
        gamepad.updateDesktopKeyboard();
        if (gamepad.read().left_trigger == 65535) trigger_seen = true;
        renderer.present();
        if (worker_done && mouse_seen && trigger_seen &&
            renderer.successfulPresentCount() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join();
    if (!worker_ok || !mouse_seen || !trigger_seen ||
        renderer.successfulPresentCount() == 0) {
        std::fprintf(stderr, "SDL test failed: worker=%d mouse=%d trigger=%d presents=%llu\n",
                     worker_ok.load(), mouse_seen, trigger_seen,
                     static_cast<unsigned long long>(renderer.successfulPresentCount()));
        return 1;
    }
    std::puts("Desktop SDL event, input and presentation test passed");
    return 0;
}
