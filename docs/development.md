# LunarNX Development Guide

This document contains build, validation, architecture, and simulator information for contributors. The project README is intentionally focused on installation and use.

## Development Priorities

- Real Nintendo Switch hardware is the compatibility target; simulator results are supporting evidence only.
- Build Switch-targeted code and dependencies inside the pinned devkitA64 Docker image, not with macOS system toolchains.
- Keep the active WebRTC provider on the tracked legacy libpeer path unless a change explicitly targets another provider.
- Keep startup light and keep the Switch NRO BSS below 32 MiB.
- Never commit tokens, authentication files, logs, simulator data, or generated build artifacts.

## Prerequisites

- Git
- Docker
- macOS or Linux as the host operating system
- The `devkitpro/devkita64:20251117` Docker image

## Prepare Dependencies

```sh
git clone https://github.com/thinkzhou/LunarNX.git
cd LunarNX
./scripts/setup_dependencies.sh
```

The setup script fetches the pinned Borealis and legacy libpeer revisions, then applies the tracked Borealis patch, the ordered legacy libpeer patch series, and the nested libsrtp, Mbed TLS, and usrsctp Switch compatibility patches. The local dependency checkouts under `lib/` are intentionally ignored by the main repository; reproducible changes belong in the tracked patch files under `tools/`.

## Build The Switch NRO

The full build wrapper rebuilds Switch dependencies and LunarNX inside Docker:

```sh
./scripts/docker_build_full.sh
```

The equivalent application-only build is:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkita64:20251117 bash -lc '
    set -e
    export DEVKITPRO=/opt/devkitpro
    export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
    make -f Makefile.switch clean
    make -f Makefile.switch -j$(nproc) \
      NETWORK_DIAG=0 CURL_PROVIDER=moonlight CURL_VERIFY=0 \
      CURL_VERBOSE=0 CURL_TIMEOUT_MS=30000
  '
```

The resulting application is written to `build/switch/LunarNX.nro`.

The combined Xbox/PlayStation build must use `CURL_PROVIDER=moonlight`. That provider supplies the curl 8.x WebSocket support required by PSN signaling; the devkitPro/wiliwili curl 7.69 path cannot open the required `wss://` connection. The default release build uses `IPV6=0`. Pass `IPV6=1` when testing native IPv6 paths. This does not add TURN relay support.

## Desktop Development

The desktop target currently provides an Xbox command-line streaming runner
with an SDL video/audio window. The Windows target is Xbox-first and does not
include the Switch UI or PlayStation workflows; existing macOS/Linux desktop
diagnostic workflows are unchanged.

```sh
make -f Makefile.desktop
make -f Makefile.desktop auth_test
make -f Makefile.desktop stream_tests
make -f Makefile.desktop xcloud_session_support_test
```

Additional diagnostic targets include `sdp_probe`, `xcloud_handshake_probe`, and `xcloud_stream`.

### Windows Xbox CLI

This workspace is configured for the installed Visual Studio Community 2022
x64 MSVC toolchain, Windows SDK, and vcpkg at `VCPKG_ROOT`. VS Code's CMake
Tools extension can select the checked-in `windows-msvc` configure preset and
`windows-msvc-release` build preset. The Visual Studio CMake generator locates
MSVC automatically; `cl.exe` does not need to be added to the regular terminal
`PATH`.

If configuring from a terminal, use PowerShell with CMake and Git for Windows
installed. Set `VCPKG_ROOT` if it is not already set. Fetch the pinned local
dependencies from Git Bash:

```sh
bash scripts/setup_dependencies.sh
```

Then configure and build:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
```

The vcpkg manifest installs SDL2, FFmpeg, Opus, curl, OpenSSL, and libsrtp.
Mbed TLS is built from libpeer's patched submodule so DTLS-SRTP remains enabled.
Run `build/windows-msvc/Release/LunarNX_Desktop.exe` from a console. The
current CLI flow supports Xbox Remote Play. Xbox Cloud and PlayStation flows,
along with the full graphical application, are not part of this first Windows
target.

On startup, the Windows CLI loads `token.json` from its profile directory and
reuses the saved credentials, refreshing them when needed. It starts Microsoft
device-code sign-in only when no usable saved credentials are found.

For multiple processes, use a different account profile for each process and
select the console by its exact name or ID. For example, run each command in a
separate PowerShell window:

```powershell
& .\build\windows-msvc\Release\LunarNX_Desktop.exe --profile account1 --console "Living Room Xbox"
& .\build\windows-msvc\Release\LunarNX_Desktop.exe --profile account2 --console "Bedroom Xbox"
```

The first launch of each profile requires Microsoft device-code sign-in.
Subsequent launches reuse its saved credentials. A profile stores its tokens,
logs, and caches under `<exe directory>\Profile\<name>` and can only be used
by one process at a time. Launching the executable from another working
directory still uses the same profile. Profile names are local labels, not
Microsoft account credentials. They may contain up to 64 ASCII letters, digits,
`_`, `-`, `.`, `@`, and `+`, but must not end in `.` or be a reserved Windows
device name. A name appears in the
directory path, so use a non-identifying label if preferred. Use a different
Microsoft account in each concurrent profile: the lock cannot detect the same
account signed into different profiles, and starting a session may clean up
existing sessions for that account.
Protect these directories and do not share or commit their token files.

The console list prints IDs. If names are duplicated, specify the ID instead.
Omit `--console` to select interactively, or omit both options to retain the
interactive behavior. To reuse an existing
login in a new profile, close LunarNX and place the existing `token.json` in
that profile's directory; do not share the same credentials between concurrently
running profiles. Profiles created by earlier builds under the working directory
or `%LOCALAPPDATA%\LunarNX\profiles\profile-<name>` are not moved automatically.

Desktop streaming keyboard controls:

```text
F/R/X/Y = A/B/X/Y    N = Xbox Guide    M = Menu    V = View    Esc = Stop
WASD = left stick    IJKL = right stick    Arrow keys = D-pad
1/2 = LB/RB    3/4 = LT/RT    5/6 = L3/R3
```

### Windows automation pipe

Pass `--automation`, `--profile NAME`, and `--pipe-name NAME` to expose a local
Windows named pipe for a separate controller process. `--pipe-name` takes the
pipe's short name, without the `\\.\pipe\` prefix. For example,
`--pipe-name LunarNX-account1` creates `\\.\pipe\LunarNX-account1`; the exact
path is also printed at startup. Choose a different pipe name for each
concurrently running LunarNX process. The pipe accepts newline-delimited
UTF-8 JSON requests.
The option is explicit and is disabled by default. It starts in Script input
mode; press **F8** in the streaming window to switch between Script and Manual
keyboard input. The named pipe and screenshot requests remain available in
either mode. Switching modes releases the current input, and Script mode
requires a fresh pipe input after switching back. Pipe input is ignored in
Manual mode. A state not refreshed for 250 ms is automatically released, and
disconnecting the pipe client also releases all controls.

Supported requests:

```json
{"type":"input","state":{"a":true,"left_stick_x":12000,"left_trigger":50000}}
{"type":"release"}
{"type":"screenshot"}
```

`input` replaces the complete state: omitted button fields are false and
omitted axes/triggers are zero. Button fields use the Xbox names (`a`, `b`,
`x`, `y`, `dpad_up/down/left/right`, `lb`, `rb`, `lt`, `rt`, `l3`, `r3`,
`view`, `menu`, `guide`, `touchpad`). Stick fields are signed 16-bit integers;
trigger fields are unsigned 16-bit integers. Each request receives a JSON
response line. A successful screenshot response includes `format: "RGB24"`,
`width`, `height`, and `bytes`; exactly `bytes` tightly packed RGB24 bytes
follow the response newline. Screenshots are taken from the SDL window on its
UI thread and require a presented video frame. Only local pipe clients are
accepted. The pipe should be treated as a control interface for the logged-in
Xbox session; do not expose it through a network proxy.

The automation script should connect to the exact pipe path printed by the
LunarNX process. Example PowerShell startup and Python connection:

```powershell
& .\build\windows-msvc\Release\LunarNX_Desktop.exe `
  --profile account1 --console "Living Room Xbox" `
  --automation --pipe-name LunarNX-account1
```

```python
pipe = open(r"\\.\pipe\LunarNX-account1", "r+b", buffering=0)
pipe.write(b'{"type":"input","state":{"a":true}}\n')
response = pipe.readline()
```

Build and run the Windows pipe regression test with:

```powershell
cmake --build --preset windows-msvc-release --target DesktopAutomationTest
python -X utf8 tests\desktop_automation_test.py
```

For the Windows SDL window, input, and presentation regression test:

```powershell
cmake --build --preset windows-msvc-release --target DesktopSdlTest
.\build\windows-msvc\Release\DesktopSdlTest.exe
cmake --build --preset windows-msvc-release --target DesktopOptionsTest
.\build\windows-msvc\Release\DesktopOptionsTest.exe
cmake --build --preset windows-msvc-release --target DesktopAutomationTest
python -X utf8 tests\desktop_automation_test.py
python -X utf8 tests\desktop_profiles_test.py
```

The dependency setup applies the tracked `0004-windows-winsock.patch` to the
legacy libpeer checkout. The Windows target initializes Winsock, uses
`GetAdaptersAddresses()` for host ICE candidates, and currently uses libpeer's
built-in SCTP implementation rather than usrsctp.

## Validation

Run the focused tests for the changed area. WebRTC, session, RTP, data-channel, or stream-startup changes require at least:

```sh
scripts/check_stream_regressions.sh
python3 tests/xbox_stream_session_order_test.py
python3 tests/libpeer_sctp_config_test.py
python3 tests/libpeer_dtls_read_loop_test.py
python3 tests/datachannel_ppid_test.py
python3 tests/switch_nro_bss_test.py
git diff --check
```

Switch source or build changes require a clean Docker build before completion. Streaming changes should receive a Ryubing mock-stream smoke test and, whenever possible, real-hardware testing.

If GLSL shaders change, rebuild the generated `.dksh` files inside a devkitPro environment:

```sh
./scripts/compile_shaders.sh
```

## Simulator Testing

Use Ryubing Canary 1.3.333 and follow [ryujinx_testing.md](ryujinx_testing.md). Prefer the application log at:

```text
$HOME/work/self/ryujinx-data/sdcard/switch/LunarNX/lunarnx.log
```

Avoid collecting long raw emulator stdout/stderr logs because they can grow very large. The simulator can validate startup, UI flow, local mock WebRTC, RTP/SRTP behavior, and the basic media pipeline, but real Switch hardware remains the final compatibility target.

## Architecture

```text
                         Borealis UI
               platform, library, console lists
                              |
               +--------------+--------------+
               |                             |
        Xbox application path         PlayStation path
    Auth + GSSV/REST Sessions       Discovery + PSN + Chiaki
    legacy libpeer WebRTC           local/remote connection plan
    ICE/DTLS-SRTP/DataChannel       Takion media/control transport
               |                             |
               +--------------+--------------+
                              |
                    shared MediaPipeline
             H.264/HEVC + Opus + NVDEC/Audren
                              |
                    deko3d presentation
```

| Path | Responsibility |
| --- | --- |
| `src/auth/` | Microsoft/Xbox authentication and token storage |
| `src/api/` | HTTP, console discovery, cloud catalog, and Xbox session APIs |
| `src/app/` | Stream profiles, session lifecycle, SDP/ICE, channels, and controller orchestration |
| `src/webrtc/` | LunarNX wrapper around the active legacy libpeer checkout |
| `src/ps/` | PlayStation discovery, pairing, PSN, Chiaki sessions, input, and haptics |
| `src/stream/` | H.264/HEVC/Opus decoding, rendering, audio, synchronization, and statistics |
| `src/input/` | Switch controller input, Xbox packet encoding, and rumble |
| `src/ui/` | Borealis activities, settings, lists, stream view, and overlays |
| `tools/mock_xbox/` | Local mock Xbox WebRTC server used for simulator tests |

## Technical References

- [Ryubing/Ryujinx workflow](ryujinx_testing.md)
- [WebRTC streaming latency audit](webrtc-streaming-latency-audit-2026-07-26.md)
- [Remote streaming ICE investigation](remote-streaming-ice-investigation-2026-07-13.md)
- [Xbox + PlayStation architecture](xbox_playstation_dual_streaming_design.md)
- [Legacy libpeer patch notes](../tools/libpeer_legacy/README.md)
- [Dependency patch maintenance backlog](dependency-patch-maintenance-backlog.md)
- [Technical plan](TECHNICAL_PLAN.md)

Read [CONTRIBUTING.md](../CONTRIBUTING.md) before opening a pull request and [SECURITY.md](../SECURITY.md) before sharing logs or reporting a vulnerability.
