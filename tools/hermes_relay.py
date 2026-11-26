#!/usr/bin/env python3
"""hermes_relay.py — PC-side inference relay for the Hermes console agent.

The Xbox guest speaks plain HTTP (no TLS in the Tiramisu guest today). This
relay terminates TLS to a real OpenAI-compatible provider, or runs a scripted
`--mock` policy so the full tool loop can be verified without any API key.

    python3 tools/hermes_relay.py --port 8790            # real provider
    python3 tools/hermes_relay.py --port 8790 --mock     # offline test

Env:
    HERMES_API_KEY   bearer token for the upstream provider
    HERMES_UPSTREAM  full chat-completions URL
                     (default https://api.openai.com/v1/chat/completions)

Endpoints:
    POST /v1/chat/completions   proxy (or mock) OpenAI chat completions
    GET  /health                liveness
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UPSTREAM = os.environ.get("HERMES_UPSTREAM", "https://api.openai.com/v1/chat/completions")
API_KEY = os.environ.get("HERMES_API_KEY", "")
if not API_KEY:
    _kf = os.environ.get("HERMES_API_KEY_FILE", "")
    if _kf:
        try:
            with open(os.path.expanduser(_kf), "r", encoding="utf-8") as _f:
                API_KEY = _f.read().strip()
        except OSError:
            pass
MAX_BODY = 8 * 1024 * 1024
MOCK = False


def _call(cid, name, args):
    return {
        "id": cid,
        "type": "function",
        "function": {"name": name, "arguments": json.dumps(args)},
    }


def mock_completion(req):
    msgs = req.get("messages") or []
    tool_msgs = [m for m in msgs if m.get("role") == "tool"]
    n = len(tool_msgs)
    task = ""
    for m in msgs:
        if m.get("role") == "user":
            task = str(m.get("content") or "")
            break

    if n == 0:
        tcs = [_call("call_list", "list_dir", {"path": "."})]
        content = None
    elif n == 1:
        tcs = [_call("call_write", "write_code", {
            "path": "hermes-proof",
            "language": "python",
            "content": "print(\"Hermes ran natively on this console.\")\n"
                       "# task: " + task[:400] + "\n",
        })]
        content = None
    elif n == 2:
        tcs = [_call("call_read", "read_file", {"path": "hermes-proof.py"})]
        content = None
    else:
        tcs = None
        parts = []
        for i, m in enumerate(tool_msgs):
            parts.append("-- tool %d --\n%s" % (i + 1, str(m.get("content", ""))[:1500]))
        content = ("Hermes tool loop complete on the console.\n"
                   "steps: %d tool calls\n%s" % (n, "\n".join(parts)))

    msg = {"role": "assistant", "content": content}
    if tcs:
        msg["tool_calls"] = tcs
    return {
        "id": "chatcmpl-mock-%d" % int(time.time()),
        "object": "chat.completion",
        "created": int(time.time()),
        "model": req.get("model", "mock"),
        "choices": [{"index": 0, "message": msg, "finish_reason": "tool_calls" if tcs else "stop"}],
        "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0},
    }


def upstream_completion(raw):
    headers = {"Content-Type": "application/json", "User-Agent": "hermes-relay/1.0 (tiramisu)"}
    if API_KEY:
        headers["Authorization"] = "Bearer " + API_KEY
    try:
        r = urllib.request.Request(UPSTREAM, data=raw, headers=headers, method="POST")
        with urllib.request.urlopen(r, timeout=120) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # noqa: BLE001 - relay must answer, not crash
        return 502, json.dumps({"error": {"message": "relay upstream failure: %s" % e}}).encode()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[relay] %s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, payload, ctype="application/json"):
        if isinstance(payload, str):
            payload = payload.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except BrokenPipeError:
            pass

    def do_GET(self):
        if self.path.split("?")[0] in ("/health", "/"):
            self._send(200, json.dumps({
                "ok": True, "mock": MOCK, "upstream": UPSTREAM,
                "key": bool(API_KEY),
            }))
        else:
            self._send(404, json.dumps({"error": "not found"}))

    def do_POST(self):
        if self.path.split("?")[0] != "/v1/chat/completions":
            self._send(404, json.dumps({"error": "not found"}))
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            self._send(411, json.dumps({"error": "bad content-length"}))
            return
        raw = self.rfile.read(n)
        try:
            req = json.loads(raw)
        except Exception as e:  # noqa: BLE001
            sys.stderr.write("[relay] BAD BODY (%d bytes): %r\n" % (len(raw), raw[:900]))
            self._send(400, json.dumps({"error": "bad json: %s" % e}))
            return

        t0 = time.time()
        if MOCK:
            out = mock_completion(req)
            body = json.dumps(out).encode()
            code = 200
        else:
            if not API_KEY:
                self._send(503, json.dumps({"error": "HERMES_API_KEY is not set on the relay"}))
                return
            code, body = upstream_completion(raw)

        n_tools = 0
        try:
            msg = json.loads(body)["choices"][0]["message"]
            n_tools = len(msg.get("tool_calls") or [])
        except Exception:  # noqa: BLE001
            pass
        sys.stderr.write("[relay] %s -> %d bytes, %d tool_call(s), %.2fs\n" % (
            self.path, len(body), n_tools, time.time() - t0))
        self._send(code, body)


def main():
    global MOCK
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--mock", action="store_true", help="scripted tool calls, no API key")
    a = ap.parse_args()
    MOCK = a.mock
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print("hermes relay on http://%s:%d  mock=%s  upstream=%s" % (
        a.host, a.port, MOCK, UPSTREAM))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
