#!/usr/bin/env python3
"""A minimal OpenAI compatible chat completions server for local testing.

It does not run a model. It echoes back canned Chinese sentences so the rest of
the pipeline -- request shape, parsing, sanitizing, candidate insertion -- can be
exercised without a GPU or a download.

Usage:
    ./scripts/mock_server.py --port 8899
    ./scripts/mock_server.py --port 8899 --reply 我们明天见 --delay 0.3

The reply is chosen from a small table keyed by the pinyin in the request, which
makes the smoke test deterministic.
"""

import argparse
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# pinyin -> candidates, which the server returns one per line.
REPLIES = {
    "nihao": ["你好"],
    # 他/她 cannot be told apart from pinyin alone, which is exactly the case
    # where offering several candidates beats offering one.
    "ta'men'zheng'zai'kai'hui": ["他们正在开会", "她们正在开会"],
    "jin'tian'tian'qi'bu'cuo": ["今天天气不错"],
    "wo'zai'xue'xi": ["我在学习"],
    "nihao'ya": ["你好呀"],
    "nhsj": ["你换手机"],
    "ceshi": ["测试"],
}
DEFAULT_REPLY = ["测试成功"]

# Pinyin keys look like ni'hao, nihao or nhsj; pull them out of the user turn.
PINYIN_RE = re.compile(r"^[a-zA-Z']+$")


def extract_pinyin(user_content):
    """Find the pinyin key in the rendered prompt template.

    The template is "{context}\\n{pinyin}" by default, so the pinyin is the last
    line that looks like keys. A context line may itself contain pinyin, so this
    scans backwards and takes the first plausible match.
    """
    for line in reversed(user_content.splitlines()):
        candidate = line.strip().replace(" ", "'")
        if candidate and PINYIN_RE.match(candidate):
            return candidate.lower()
    return ""


def make_reply(pinyin):
    for key, candidates in REPLIES.items():
        if key == pinyin:
            return candidates
    return DEFAULT_REPLY


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[mock]", fmt % args, flush=True)

    def do_POST(self):
        if not self.path.endswith("/v1/chat/completions"):
            self.send_error(404, "expected /v1/chat/completions")
            return

        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            payload = json.loads(raw or b"{}")
            messages = payload.get("messages") or []
            user = ""
            for message in reversed(messages):
                if message.get("role") == "user":
                    user = message.get("content") or ""
                    break
        except json.JSONDecodeError:
            self.send_error(400, "invalid json")
            return

        pinyin = extract_pinyin(user)
        candidates = make_reply(pinyin)
        content = "\n".join(candidates)

        print(f"[mock] pinyin={pinyin!r} -> {content!r}", flush=True)

        if self.server.delay:
            time.sleep(self.server.delay)

        body = json.dumps(
            {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "model": payload.get("model", "mock"),
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": content},
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": len(user),
                    "completion_tokens": len(content),
                    "total_tokens": len(user) + len(content),
                },
            }
        ).encode()

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # A tiny health endpoint so smoke.sh can wait for readiness.
        if self.path.endswith("/healthz"):
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--delay",
        type=float,
        default=0.0,
        help="artificial response delay in seconds, to exercise the UI path",
    )
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.delay = args.delay
    print(
        f"[mock] listening on http://{args.host}:{args.port}/v1/chat/completions",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
