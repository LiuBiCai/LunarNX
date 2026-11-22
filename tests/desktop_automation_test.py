#!/usr/bin/env python3
import json
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from ctypes import wintypes


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "windows-msvc" / "Release"
EXE = BUILD / "DesktopAutomationTest.exe"


class NamedPipeClient:
    def __init__(self, handle):
        self.handle = handle

    def write(self, data):
        offset = 0
        while offset < len(data):
            chunk = data[offset:]
            buffer = ctypes.create_string_buffer(chunk)
            written = wintypes.DWORD()
            if not kernel32.WriteFile(
                self.handle, buffer, len(chunk), ctypes.byref(written), None
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            if written.value == 0:
                raise RuntimeError("Named pipe write made no progress")
            offset += written.value

    def read(self, size):
        buffer = ctypes.create_string_buffer(size)
        received = wintypes.DWORD()
        if not kernel32.ReadFile(
            self.handle, buffer, size, ctypes.byref(received), None
        ):
            error = ctypes.get_last_error()
            if error == 109:  # ERROR_BROKEN_PIPE
                return b""
            raise ctypes.WinError(error)
        return buffer.raw[:received.value]

    def readline(self):
        result = bytearray()
        while True:
            char = self.read(1)
            if not char:
                raise RuntimeError("Named pipe closed before response line")
            if char == b"\n":
                return bytes(result)
            result.extend(char)

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None


def read_exact(pipe, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = pipe.read(remaining)
        if not chunk:
            raise RuntimeError("Named pipe closed during screenshot transfer")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
kernel32.WaitNamedPipeW.restype = wintypes.BOOL
kernel32.ReadFile.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [
    wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID,
]
kernel32.WriteFile.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def main():
    if sys.platform != "win32":
        print("Desktop automation pipe test skipped: Windows required")
        return
    if not EXE.is_file():
        raise RuntimeError("Build the DesktopAutomationTest target first")

    pipe_name = r"\\.\pipe\LunarNX-Test-" + uuid.uuid4().hex
    env = os.environ.copy()
    env["PATH"] = str(BUILD) + os.pathsep + env.get("PATH", "")
    process = subprocess.Popen(
        [str(EXE), pipe_name], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=env,
    )
    client = None
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError("Automation test process exited before pipe connected")
            handle = kernel32.CreateFileW(
                pipe_name, 0xC0000000, 0, None, 3, 0, None
            )
            if handle != wintypes.HANDLE(-1).value:
                client = NamedPipeClient(handle)
                break
            error = ctypes.get_last_error()
            if error not in (2, 231):
                raise ctypes.WinError(error)
            if error == 231:
                kernel32.WaitNamedPipeW(pipe_name, 100)
            else:
                time.sleep(0.05)
        if client is None:
            raise RuntimeError("Timed out connecting to automation named pipe")

        state = {
            "a": True,
            "left_stick_x": 1234,
            "left_stick_y": -2345,
            "left_trigger": 54321,
        }
        client.write(json.dumps({"type": "input", "state": state}).encode() + b"\n")
        assert json.loads(client.readline()) == {"ok": True}

        client.write(b'{"type":"screenshot"}\n')
        header = json.loads(client.readline())
        assert header["ok"] and header["format"] == "RGB24"
        assert header["width"] == 320 and header["height"] == 240
        pixels = read_exact(client, header["bytes"])
        assert len(pixels) == 320 * 240 * 3 and any(pixels)

        client.write(
            b'{"type":"input","state":{"left_stick_x":32768}}\n'
        )
        assert json.loads(client.readline()) == {
            "ok": False, "error": "invalid_state"
        }
        time.sleep(0.4)

        client.write(b'{"type":"release"}\n')
        assert json.loads(client.readline()) == {"ok": True}
        client.close()
        client = None
        stdout, stderr = process.communicate(timeout=10)
        if process.returncode:
            raise RuntimeError(
                f"automation test exited {process.returncode}\n"
                f"stdout={stdout.decode(errors='replace')}\n"
                f"stderr={stderr.decode(errors='replace')}"
            )
        print("Desktop automation pipe tests passed")
    finally:
        if client is not None:
            client.close()
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)


if __name__ == "__main__":
    main()
