#!/usr/bin/env python3
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / "windows-msvc" / "Release" / "LunarNX_Desktop.exe"


def main():
    if sys.platform != "win32":
        print("Desktop profile integration test skipped: Windows required")
        return
    if not EXE.is_file():
        raise RuntimeError("Build the Windows Release application before running this test")

    requests = queue.Queue()
    release = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{"sessions":[]}')

        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            requests.put(json.loads(body))
            if release.wait(20):
                return
            self.send_response(400)
            self.end_headers()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    children = []
    tmp = tempfile.TemporaryDirectory(prefix="lunarnx-profile-test-")
    try:
        base = Path(tmp.name)
        app_dir = base / "application"
        app_dir.mkdir()
        test_exe = app_dir / EXE.name
        shutil.copy2(EXE, test_exe)
        other_cwd = base / "\u6d4b\u8bd5"
        other_cwd.mkdir()
        env = os.environ.copy()
        env["PATH"] = str(EXE.parent) + os.pathsep + env.get("PATH", "")
        env["LUNARNX_MOCK_URL"] = f"http://127.0.0.1:{server.server_port}"

        def start(profile, console):
            proc = subprocess.Popen(
                [str(test_exe), "--profile", profile, "--console", console],
                cwd=base, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            children.append(proc)
            return proc

        first = start("alpha+1@example.invalid", "Mock-Xbox-Series-X")
        second = start("beta", "MOCKXBOX001")
        for _ in range(2):
            assert requests.get(timeout=15)["serverId"] == "MOCKXBOX001"
        assert first.poll() is None and second.poll() is None
        profiles = app_dir / "Profile"
        assert (profiles / "alpha+1@example.invalid").is_dir()
        assert (profiles / "beta").is_dir()
        assert Path(first.args[0]).parent == app_dir

        duplicate = subprocess.run(
            [str(test_exe), "--profile", "alpha+1@example.invalid", "--console", "MOCKXBOX001"],
            cwd=other_cwd, env=env, capture_output=True, timeout=10,
        )
        assert duplicate.returncode != 0 and b"already in use" in duplicate.stderr
        missing = subprocess.run(
            [str(test_exe), "--profile", "gamma", "--console", "missing"],
            cwd=base, env=env, capture_output=True, timeout=10,
        )
        assert missing.returncode != 0 and b"Console not found" in missing.stderr
        no_profile = subprocess.run(
            [str(test_exe), "--console", "missing"],
            cwd=base, env=env, capture_output=True, timeout=10,
        )
        assert no_profile.returncode != 0 and b"Console not found" in no_profile.stderr
        assert (base / ".lunarnx.lock").is_file()
        assert requests.empty()

        bad_dir = profiles / "invalid"
        bad_dir.mkdir()
        token = bad_dir / "token.json"
        token.write_text("{invalid", encoding="utf-8")
        auth_env = env.copy()
        del auth_env["LUNARNX_MOCK_URL"]
        invalid = subprocess.run(
            [str(test_exe), "--profile", "invalid"],
            cwd=base, env=auth_env, capture_output=True, timeout=10,
        )
        assert invalid.returncode != 0 and b"unreadable or invalid" in invalid.stderr
        assert token.read_text(encoding="utf-8") == "{invalid"

        for proc in children:
            proc.terminate()
            proc.communicate(timeout=10)
        children.clear()
        reopened = subprocess.run(
            [str(test_exe), "--profile", "alpha+1@example.invalid", "--console", "missing"],
            cwd=base, env=env, capture_output=True, timeout=10,
        )
        assert reopened.returncode != 0 and b"Console not found" in reopened.stderr
    finally:
        for proc in children:
            proc.terminate()
            proc.communicate(timeout=10)
        release.set()
        server.shutdown()
        server.server_close()
        thread.join()
        tmp.cleanup()
    print("Desktop profile integration tests passed")


if __name__ == "__main__":
    main()
