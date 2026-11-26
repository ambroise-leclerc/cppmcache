#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc
"""A minimal server of Bazel's HTTP caching protocol, for the tests of the remote cache.

It stores blobs in memory under /ac/<sha256> and /cas/<sha256> and answers GET, HEAD and PUT.
Like bazel-remote, it refuses a CAS blob whose content does not match its name.

    CacheServer.py -- <command> [arguments...]

starts the server on a free local port, runs the command with CPPMCACHE_TEST_REMOTE set to the
server's URL, and exits with the command's status. If CPPMCACHE_TEST_REMOTE is already set, no
server is started and the command talks to that one instead: this is how CI runs the same test
against a real bazel-remote.

    CacheServer.py --serve [--port N]

only serves, until interrupted.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY = re.compile(r"^/(ac|cas)/([0-9a-f]{64})$")


class CacheHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    blobs: dict[str, bytes] = {}
    lock = threading.Lock()
    chunked = False

    def log_message(self, format: str, *args: object) -> None:  # noqa: A002 - name imposed by the base class
        if os.environ.get("CPPMCACHE_TEST_SERVER_VERBOSE"):
            super().log_message(format, *args)

    def reply(self, status: int, body: bytes = b"", *, head: bool = False) -> None:
        self.send_response(status)
        if self.chunked and body and not head:
            # Exercises the client's chunked decoding: the body is sent in two chunks.
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            middle = len(body) // 2
            for part in (body[:middle], body[middle:]):
                if part:
                    self.wfile.write(f"{len(part):x}\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def lookup(self, *, head: bool) -> None:
        if not KEY.match(self.path):
            self.reply(400, head=head)
            return
        with self.lock:
            body = self.blobs.get(self.path)
        if body is None:
            self.reply(404, head=head)
        else:
            self.reply(200, body, head=head)

    def do_GET(self) -> None:  # noqa: N802 - name imposed by the base class
        self.lookup(head=False)

    def do_HEAD(self) -> None:  # noqa: N802
        self.lookup(head=True)

    def do_PUT(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        match = KEY.match(self.path)
        if not match:
            self.reply(400)
            return
        if match.group(1) == "cas" and hashlib.sha256(body).hexdigest() != match.group(2):
            self.reply(400)
            return
        with self.lock:
            self.blobs[self.path] = body
        self.reply(200)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serve", action="store_true", help="only serve, until interrupted")
    parser.add_argument("--port", type=int, default=0, help="port to listen on (default: a free one)")
    parser.add_argument("--chunked", action="store_true", help="answer GET requests with chunked bodies")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run, after --")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command

    if not args.serve and os.environ.get("CPPMCACHE_TEST_REMOTE"):
        return subprocess.run(command, check=False).returncode

    CacheHandler.chunked = args.chunked
    server = ThreadingHTTPServer(("127.0.0.1", args.port), CacheHandler)
    url = f"http://127.0.0.1:{server.server_address[1]}"
    if args.serve:
        print(url, flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        return 0
    if not command:
        parser.error("a command is required unless --serve is given")
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        environment = dict(os.environ, CPPMCACHE_TEST_REMOTE=url)
        return subprocess.run(command, env=environment, check=False).returncode
    finally:
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
