#!/usr/bin/env python3
"""Deterministic local OpenAI-compatible server used by smoke.sh.

No model or third-party Python package is needed. POST /v1/chat/completions
returns canned readings; --reply overrides them and --delay delays responses.
GET /healthz and /stats report readiness and requests already received.
POST /reset clears stats. Error/redirect/slow endpoints exercise the worker.
Never expose this unauthenticated test server outside loopback.
"""

import argparse
import json
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

REPLIES = {
    "nihao": ["你好"],
    "ni'hao": ["你好"],
    "ta'men'zheng'zai'kai'hui": ["他们正在开会", "她们正在开会"],
    "jin'tian'tian'qi'bu'cuo": ["今天天气不错"],
    "wo'zai'xue'xi": ["我在学习"],
    "nihao'ya": ["你好呀"],
    "nhsj": ["你换手机"],
    "ceshi": ["测试"],
    "shui": ["谁"],
}
PINYIN_RE = re.compile(r"^[a-zA-Z']+$")


def extract_pinyin(user):
    for line in reversed(user.splitlines()):
        candidate = line.strip().replace(" ", "'")
        if candidate and PINYIN_RE.fullmatch(candidate):
            return candidate.lower()
    return ""


def make_reply(pinyin, user):
    context = user.splitlines()[0] if "\n" in user else ""
    if pinyin == "ta'men'zheng'zai'kai'hui":
        if context == "乙丙":
            return ["她们正在开会"]
        if context == "丁戊":
            return ["它们正在开会"]
    return REPLIES.get(pinyin, ["测试成功"])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[mock]", fmt % args, flush=True)

    def respond(self, status, body, headers=None, content_type="application/json"):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.close_connection = True
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # Cancellation is an expected test result, not a server failure.
            pass

    def read_payload(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.respond(400, {"error": "invalid Content-Length"})
            return None
        if not 0 <= length <= 1024 * 1024:
            self.respond(413, {"error": "request too large"})
            return None
        raw = self.rfile.read(length)
        try:
            payload = json.loads(raw or b"{}")
            messages = payload.get("messages", [])
            user = next((m.get("content", "") for m in reversed(messages)
                         if m.get("role") == "user"), "")
            if not isinstance(user, str):
                raise ValueError("user content must be text")
        except (ValueError, AttributeError, TypeError):
            self.respond(400, {"error": "invalid JSON request"})
            return None
        record = {"path": self.path, "method": self.command,
                  "pinyin": extract_pinyin(user), "user": user,
                  "authorization": self.headers.get("Authorization", ""),
                  "body": raw.decode("utf-8", errors="replace")}
        # Record before response delay, allowing tests to synchronize cancellation.
        with self.server.stats_lock:
            self.server.requests.append(record)
        return payload, record

    def do_POST(self):
        path = urlsplit(self.path).path
        if path == "/reset":
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)
            with self.server.stats_lock:
                self.server.requests.clear()
            self.respond(200, {"ok": True})
            return
        result = self.read_payload()
        if result is None:
            return
        payload, record = result
        redirect = re.fullmatch(r"/(redirect|redirect-cross-host)/(301|302|303|307|308)", path)
        if redirect:
            host = "localhost" if redirect[1] == "redirect-cross-host" else "127.0.0.1"
            self.respond(int(redirect[2]), {}, {
                "Location": f"http://{host}:{self.server.server_port}/v1/chat/completions"})
            return
        if not path.endswith("/v1/chat/completions"):
            self.respond(404, {"error": "unknown endpoint"})
            return
        if path.startswith("/error/"):
            self.respond(500, {"error": "intentional mock failure"})
            return
        if path.startswith("/invalid/"):
            self.respond(200, b"not JSON")
            return
        content = self.server.reply
        if content is None:
            content = "\n".join(make_reply(record["pinyin"], record["user"]))
        finish_reason = "stop"
        if path.startswith("/empty/"):
            content = ""
        elif path.startswith("/all-filtered/"):
            # Valid completion envelope, but no candidate survives sanitizing:
            # Latin output, incorrect Han length and punctuation-only content.
            content = "not a Chinese candidate\n测试\n。。。"
        elif path.startswith("/late-overlap/"):
            # With MaxCandidates=1 only the first novel reading is displayed.
            # The later native reading must still make this request overlap;
            # trailing punctuation is sanitized before exact comparison.
            content = "她们正在开会\n他们正在开会。\n它们正在开会"
        elif path.startswith("/truncated/"):
            content = "你好"
            finish_reason = "length"
        elif path.startswith("/oversized/"):
            content = "测" * 70000
        delay = 3.0 if path.startswith("/slow/") else self.server.delay
        print(f"[mock] pinyin={record['pinyin']!r} path={path} delay={delay}", flush=True)
        if delay:
            time.sleep(delay)
        self.respond(200, {
            "id": "chatcmpl-mock", "object": "chat.completion",
            "model": payload.get("model", "mock"),
            "choices": [{"index": 0, "message": {
                "role": "assistant", "content": content},
                "finish_reason": finish_reason}],
        })

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/healthz":
            self.respond(200, {"ok": True, "token": self.server.token,
                               "port": self.server.server_port,
                               "dead_port": self.server.dead_socket.getsockname()[1]})
        elif path == "/stats":
            with self.server.stats_lock:
                requests = list(self.server.requests)
            self.respond(200, {"count": len(requests), "requests": requests})
        else:
            # Record unexpected GET redirects so method preservation is testable.
            if path.endswith("/v1/chat/completions"):
                with self.server.stats_lock:
                    self.server.requests.append({"path": self.path, "method": "GET",
                        "authorization": self.headers.get("Authorization", ""),
                        "pinyin": "", "user": "", "body": ""})
            self.respond(404, {"error": "unknown GET endpoint"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8899,
                        help="0 chooses a free ephemeral port")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--delay", type=float, default=0.0)
    parser.add_argument("--reply", help="override the canned reply")
    parser.add_argument("--token", default="", help="readiness identity for smoke.sh")
    parser.add_argument("--ready-file", help="write JSON readiness after successful bind")
    args = parser.parse_args()
    if args.delay < 0:
        parser.error("--delay must be nonnegative")
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    # Reserve a port without listening: connections reliably fail while this
    # process owns it, rather than assuming port 9 happens to be unused.
    server.dead_socket = socket.socket()
    server.dead_socket.bind(("127.0.0.1", 0))
    server.delay, server.reply, server.token = args.delay, args.reply, args.token
    server.requests, server.stats_lock = [], threading.Lock()
    ready = {"port": server.server_port, "token": args.token}
    if args.ready_file:
        with open(args.ready_file, "w", encoding="utf-8") as stream:
            json.dump(ready, stream)
    print(f"[mock] listening on http://{args.host}:{server.server_port}/v1/chat/completions", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        server.dead_socket.close()


if __name__ == "__main__":
    main()
