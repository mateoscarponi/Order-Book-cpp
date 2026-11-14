"""Local web demo for the C++ order book.

Starts the compiled `interactive` program and exposes it to the browser:
    GET  /              -> the demo page (index.html)
    POST /api/command   -> {"command": "NEW BUY LIMIT 15005 100"} forwarded to the engine

Only the Python standard library is used. The server listens on 127.0.0.1 only.

usage:
    python demo/server.py                      # looks for build/interactive(.exe)
    python demo/server.py --port 8000 --binary path/to/interactive
"""

import argparse
import json
import pathlib
import subprocess
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
ALLOWED = {"NEW", "CANCEL", "MODIFY", "BOOK", "RESET"}
MAX_BODY = 1024


def find_binary(explicit):
    if explicit:
        return pathlib.Path(explicit)
    for candidate in ("build/interactive", "build/interactive.exe",
                      "build/Release/interactive.exe", "build/Debug/interactive.exe"):
        path = ROOT / candidate
        if path.exists():
            return path
    sys.exit("Could not find the 'interactive' binary. Build first:\n"
             "  cmake -S . -B build && cmake --build build\n"
             "or pass --binary path/to/interactive")


class Engine:
    """Owns the C++ process. One command at a time, one JSON line back."""

    def __init__(self, binary):
        self.lock = threading.Lock()
        self.process = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1)

    def send(self, command):
        with self.lock:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
            line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("engine process exited")
        return json.loads(line)


class Handler(SimpleHTTPRequestHandler):
    engine = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(HERE), **kwargs)

    def do_POST(self):
        if self.path != "/api/command":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_BODY:
            self.send_error(400, "bad body")
            return
        try:
            command = str(json.loads(self.rfile.read(length))["command"]).strip()
        except (ValueError, KeyError, TypeError):
            self.send_error(400, "expected {\"command\": \"...\"}")
            return
        if "\n" in command or "\r" in command or command.split(" ")[0] not in ALLOWED:
            self.send_error(400, "unknown command")
            return
        try:
            body = json.dumps(self.engine.send(command)).encode()
        except RuntimeError as error:
            self.send_error(500, str(error))
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        if self.command == "POST":
            return  # keep the console quiet while orders stream in
        super().log_message(fmt, *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--binary")
    args = parser.parse_args()

    Handler.engine = Engine(find_binary(args.binary))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Order book demo running at http://localhost:{args.port}  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        Handler.engine.process.terminate()


if __name__ == "__main__":
    main()
