#!/usr/bin/env python3
"""Watchy emulator server.

    python3 server.py [face-folder] [--port 8077]

Builds ../faces/<face-folder> with build.sh, runs it as a child process and serves the browser UI.
Any change to the face folder or the emulator sources triggers a rebuild; the page
notices the new version and reboots the emulated watch.

The emulator runs in real time: frames are streamed to the page as the face draws them
(/api/frames), and the page can steer the accelerometer meanwhile (/api/accel).
"""
import argparse
import json
import os
import queue
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WATCHED_EXTS = (".h", ".cpp", ".c", ".ino")


class Emulator:
    def __init__(self, face):
        self.face = face
        self.lock = threading.Lock()
        self.proc = None
        self.replies = queue.Queue()
        self.stream = []  # (seq, item) for frames/vibes, newest last
        self.stream_seq = 0
        self.stream_lock = threading.Lock()
        self.accel_file = os.path.join(HERE, "build", f"accel-{face}.txt")
        self.set_accel(0, 0, -1024)
        self.version = 0
        self.building = False
        self.error = None

    def set_accel(self, x, y, z):
        os.makedirs(os.path.dirname(self.accel_file), exist_ok=True)
        tmp = f"{self.accel_file}.{threading.get_ident()}.tmp"  # requests can overlap
        with open(tmp, "w") as f:
            f.write(f"{int(x)} {int(y)} {int(z)}\n")
        os.replace(tmp, self.accel_file)

    def read_output(self, proc):
        # streamed frames/vibes go to the page as they arrive; anything else answers a command
        for line in proc.stdout:
            msg = json.loads(line)
            if "frame" in msg or "vibe" in msg:
                with self.stream_lock:
                    self.stream_seq += 1
                    self.stream.append((self.stream_seq, msg))
                    del self.stream[:-300]
            else:
                self.replies.put(msg)

    def frames_after(self, seq):
        with self.stream_lock:
            return self.stream_seq, [dict(item, seq=s) for s, item in self.stream if s > seq]

    def watched_dirs(self):
        return [os.path.join(ROOT, "faces", self.face), os.path.join(HERE, "stubs"), os.path.join(HERE, "core")]

    def snapshot(self):
        mtimes = {}
        for d in self.watched_dirs():
            for name in os.listdir(d):
                if name.endswith(WATCHED_EXTS):
                    p = os.path.join(d, name)
                    mtimes[p] = os.path.getmtime(p)
        return mtimes

    def build(self):
        self.building = True
        started = time.time()
        result = subprocess.run([os.path.join(HERE, "build.sh"), self.face], capture_output=True, text=True)
        self.building = False
        if result.returncode != 0:
            self.error = (result.stdout + result.stderr).strip()
            print(f"build failed ({time.time() - started:.1f}s)")
            return False
        binary = result.stdout.strip().splitlines()[-1]
        with self.lock:
            if self.proc:
                self.proc.kill()
            env = dict(os.environ, EMU_REALTIME="1", EMU_ACCEL_FILE=self.accel_file,
                       EMU_HTTP_CACHE=os.path.join(HERE, "build", "http-cache"))
            self.proc = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, env=env)
            self.replies = queue.Queue()
            threading.Thread(target=self.read_output, args=(self.proc,), daemon=True).start()
            self.error = None
            self.version += 1
        print(f"built {self.face} v{self.version} ({time.time() - started:.1f}s)")
        return True

    def command(self, line):
        with self.lock:
            if not self.proc or self.proc.poll() is not None:
                return {"ok": False, "error": "emulator is not running" + (" (build failed)" if self.error else " (it crashed; edit a file to rebuild)")}
            proc, replies = self.proc, self.replies
            proc.stdin.write(line.strip() + "\n")
            proc.stdin.flush()
            while True:
                try:
                    return replies.get(timeout=1)
                except queue.Empty:
                    if proc.poll() is not None:
                        return {"ok": False, "error": "emulator crashed while handling: " + line}

    def watch(self):
        last = self.snapshot()
        while True:
            time.sleep(0.7)
            try:
                now = self.snapshot()
            except FileNotFoundError:
                continue  # editors briefly remove files while saving
            if now != last:
                last = now
                time.sleep(0.2)  # let multi-file saves settle
                last = self.snapshot()
                self.build()


def make_handler(emu):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send_json(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                with open(os.path.join(HERE, "web", "index.html"), "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path.startswith("/api/frames"):
                after = int(self.path.partition("after=")[2] or 0)
                seq, items = emu.frames_after(after)
                self.send_json({"seq": seq, "items": items})
            elif self.path == "/api/status":
                self.send_json({"face": emu.face, "version": emu.version, "building": emu.building, "error": emu.error})
            else:
                self.send_error(404)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            payload = json.loads(self.rfile.read(length) or b"{}")
            if self.path == "/api/accel":
                emu.set_accel(payload.get("x", 0), payload.get("y", 0), payload.get("z", -1024))
                self.send_json({"ok": True})
                return
            if self.path != "/api/cmd":
                self.send_error(404)
                return
            replies = [emu.command(c) for c in payload.get("cmds", [])]
            self.send_json({"version": emu.version, "replies": replies})

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("face", nargs="?", default="my-face")
    ap.add_argument("--port", type=int, default=8077)
    args = ap.parse_args()

    emu = Emulator(args.face)
    emu.build()
    threading.Thread(target=emu.watch, daemon=True).start()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(emu))
    print(f"Watchy emulator for {args.face}: http://localhost:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if emu.proc:
            emu.proc.kill()


if __name__ == "__main__":
    main()
