/**
 * LunarNX Desktop Test Runner
 *
 * Full-stack Xbox streaming test on macOS/Linux using SDL2 for video/audio/keyboard.
 * Validates the entire pipeline (auth → API → WebRTC → decode → render → input)
 * without needing Switch hardware.
 *
 * Build:
 *   mkdir -p build/pc && cd build/pc
 *   cmake ../.. -DPLATFORM_DESKTOP=ON -DCMAKE_BUILD_TYPE=Debug
 *   make LunarNX_Desktop -j$(sysctl -n hw.ncpu)
 *
 * Usage:
 *   ./LunarNX_Desktop
 *
 * Keyboard controls (during streaming):
 *   F/R/X/Y=A/B/X/Y  N=Guide  M=Menu  V=View  Esc=Stop
 *   Arrow keys=D-pad  WASD=Left stick  IJKL=Right stick
 *   1/2=LB/RB  3/4=LT/RT  5/6=L3/R3
 */

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "../app/stream_controller.h"
#include "desktop_options.h"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <signal.h>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

#ifdef _WIN32
class WinsockLifetime {
public:
    WinsockLifetime() : result_(WSAStartup(MAKEWORD(2, 2), &data_)) {}
    ~WinsockLifetime() {
        if (result_ == 0) WSACleanup();
    }

    bool initialized() const { return result_ == 0; }

private:
    WSADATA data_{};
    int result_;
};

class ProfileLock {
public:
    ~ProfileLock() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    bool acquire(const std::string& profile) {
        std::error_code ec;
        std::filesystem::path directory = std::filesystem::current_path(ec);
        if (!ec && !profile.empty()) {
            std::vector<wchar_t> executable_path(32768);
            const DWORD path_length = GetModuleFileNameW(
                nullptr, executable_path.data(),
                static_cast<DWORD>(executable_path.size()));
            if (path_length == 0 || path_length >= executable_path.size()) {
                fprintf(stderr, "ERROR: Cannot resolve executable directory (Windows error %lu).\n",
                        GetLastError());
                return false;
            }
            executable_path.resize(path_length);
            directory = std::filesystem::path(executable_path.data()).parent_path() /
                        L"Profile" / std::filesystem::path(profile);
            std::filesystem::create_directories(directory, ec);
        }
        if (ec) {
            fprintf(stderr, "ERROR: Cannot access profile directory: %s\n", ec.message().c_str());
            return false;
        }
        const auto lock_path = directory / L".lunarnx.lock";
        handle_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_SHARING_VIOLATION) {
                fprintf(stderr, "ERROR: This profile is already in use by another process.\n");
            } else {
                fprintf(stderr, "ERROR: Cannot lock profile (Windows error %lu).\n", error);
            }
            return false;
        }
        if (!SetCurrentDirectoryW(directory.c_str())) {
            fprintf(stderr, "ERROR: Cannot enter profile directory (Windows error %lu).\n",
                    GetLastError());
            return false;
        }
        if (!profile.empty()) printf("Using profile: %s\n", profile.c_str());
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
#endif

static int runDesktop(const std::vector<std::string>& args) {
    SDL_SetMainReady();
    setbuf(stdout, NULL);  // unbuffered for test output
    lunar::desktop::Options options;
    std::string option_error;
    if (!lunar::desktop::parseOptions(args, options, option_error)) {
        fprintf(stderr, "ERROR: %s\nUse --help for usage.\n", option_error.c_str());
        return 1;
    }
    if (options.help) {
        printf("Usage: LunarNX_Desktop [--profile NAME] [--console NAME_OR_ID]\n"
               "  --profile NAME       Select an isolated Windows account profile.\n"
               "  --console NAME_OR_ID Select an Xbox by exact name or ID.\n");
        return 0;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef _WIN32
    ProfileLock profile_lock;
    if (!profile_lock.acquire(options.profile)) return 1;
    WinsockLifetime winsock;
    if (!winsock.initialized()) {
        fprintf(stderr, "ERROR: Failed to initialize Windows networking.\n");
        return 1;
    }
#else
    if (!options.profile.empty()) {
        fprintf(stderr, "ERROR: --profile is currently supported on Windows only.\n");
        return 1;
    }
#endif
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    printf("=== LunarNX Desktop Test v0.1 ===\n\n");

    lunar::app::StreamController ctrl;

    ctrl.setStateCallback([](lunar::app::StreamState s, const std::string& info) {
        static const char* names[] = {"Idle","Auth","Connecting","Streaming","Disconnected","Error"};
        printf("[%s] %s\n", names[(int)s], info.c_str());
    });

    const char* mock_url = getenv("LUNARNX_MOCK_URL");

    if (mock_url && mock_url[0]) {
        // --- Mock mode: skip real auth, use local mock server ---
        printf("[mock] Bypassing auth, using mock Xbox at %s\n", mock_url);
        if (!ctrl.bypassAuthForMock(mock_url)) {
            fprintf(stderr, "ERROR: Failed to initialize mock mode.\n");
            return 1;
        }
    } else {
        // --- Normal mode: reuse saved Microsoft credentials when available ---
        std::error_code token_error;
        const bool token_exists = std::filesystem::exists(lunar::get_token_path(), token_error);
        if (token_error) {
            fprintf(stderr, "ERROR: Cannot access saved sign-in: %s\n",
                    token_error.message().c_str());
            return 1;
        }
        if (ctrl.loadTokens(lunar::get_token_path())) {
            printf("Using saved sign-in");
            if (!ctrl.getGamertag().empty()) {
                printf(" for %s", ctrl.getGamertag().c_str());
            }
            printf(".\n");
        } else {
            if (token_exists) {
                fprintf(stderr, "ERROR: Saved sign-in is unreadable or invalid; "
                                "check the profile token file before signing in again.\n");
                return 1;
            }
            printf("Starting authentication...\n");
            if (!ctrl.startAuth()) {
                fprintf(stderr, "ERROR: Failed to start authentication.\n");
                return 1;
            }

            printf("\n  Visit: %s\n", ctrl.getAuthUrl().c_str());
            printf("  Code:  %s\n\n", ctrl.getAuthCode().c_str());
            printf("Press Enter AFTER completing the browser login...\n");
            getchar();

            printf("Polling for token...\n");
            if (!ctrl.pollAuth()) {
                fprintf(stderr, "ERROR: Authentication failed.\n");
                return 1;
            }
            if (!ctrl.saveTokens(lunar::get_token_path())) {
                fprintf(stderr, "ERROR: Could not save sign-in to the profile.\n");
                return 1;
            }
            printf("Logged in as: %s\n\n", ctrl.getGamertag().c_str());
        }

        printf("Fetching console list...\n");
        if (!ctrl.fetchConsoles()) {
            const auto error = ctrl.getConsoleFetchError();
            fprintf(stderr, "ERROR: %s\n",
                    error.empty() ? "Failed to fetch Xbox consoles." : error.c_str());
            return 1;
        }
        if (!ctrl.saveTokens(lunar::get_token_path())) {
            fprintf(stderr, "ERROR: Could not save refreshed sign-in to the profile.\n");
            return 1;
        }
    }

    // Console list
    auto consoles = ctrl.getConsoles();
    for (size_t i = 0; i < consoles.size(); i++) {
        printf("  [%zu] %s  (%s)\n", i, consoles[i].name.c_str(), consoles[i].id.c_str());
    }

    size_t choice = 0;
    if (consoles.empty()) {
        fprintf(stderr, "ERROR: No Xbox consoles are available.\n");
        return 1;
    }
    if (!options.console.empty()) {
        std::string selection_error;
        if (!lunar::desktop::selectConsole(consoles, options.console, choice, selection_error)) {
            fprintf(stderr, "ERROR: %s\n", selection_error.c_str());
            return 1;
        }
    } else if (consoles.size() > 1) {
        printf("\nChoose console [0-%zu]: ", consoles.size() - 1);
        if (scanf("%zu", &choice) != 1 || choice >= consoles.size()) {
            fprintf(stderr, "ERROR: Invalid console selection.\n");
            return 1;
        }
        getchar();
    }

    // Connect & stream
    printf("\nConnecting to %s...\n", consoles[choice].name.c_str());
    if (!ctrl.startStream(consoles[choice].id, 1280, 720)) {
        fprintf(stderr, "ERROR: Failed to start stream.\n");
        return 1;
    }

    printf("\n=== STREAMING ===\n");
    printf("Keyboard: F/R/X/Y=A/B/X/Y  N=Guide  Arrows=D-pad  WASD=LStick  IJKL=RStick\n");
    printf("          1/2=LB/RB  3/4=LT/RT  5/6=L3/R3  V=View  M=Menu  Esc=Stop\n\n");

    // SDL window events, keyboard sampling, and presentation belong to this thread.
    while (g_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                g_running = false;
            }
        }
        if (!g_running) break;
        ctrl.updateDesktopKeyboard();
        ctrl.presentVideoFrame();
        ctrl.update();

        auto state = ctrl.getState();
        if (state == lunar::app::StreamState::Disconnected ||
            state == lunar::app::StreamState::Error) {
            printf("Stream ended (state=%d)\n", (int)state);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    printf("\nShutting down...\n");
    ctrl.stopStream();
    printf("Done.\n");
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i],
                                             -1, nullptr, 0, nullptr, nullptr);
        if (size == 0) {
            fprintf(stderr, "ERROR: Invalid Unicode command-line argument.\n");
            return 1;
        }
        std::string value(static_cast<size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                value.data(), size, nullptr, nullptr) == 0) {
            fprintf(stderr, "ERROR: Could not convert command-line argument.\n");
            return 1;
        }
        value.pop_back();
        args.push_back(std::move(value));
    }
    return runDesktop(args);
}
#else
int main(int argc, char* argv[]) {
    return runDesktop(std::vector<std::string>(argv, argv + argc));
}
#endif
