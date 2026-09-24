#!/usr/bin/env python3
"""Soft Ready --serve-async two successive denseness http-post batches (#4054).

Regression for lost async wake / stdin eval wedge after the first denseness
async http-post batch succeeds (MiniMax live case). Local CI substitute:
ThreadingHTTPServer stub with multi-second delay + large JSON body; run TWO
N=2 denseness fiber http-post batches on ONE Soft Ready --serve-async
session, plus intervening and post-batch (+ 1 1) pings.

Acceptance:
  - batch1 and batch2 both status=ok
  - inter_ping after batch1 and post_ping after batch2 ok promptly
  - Soft Ready banner honesty (no production Ready stamp)
  - Does NOT claim production Ready

Live MiniMax two-batch smoke remains documented on the issue close comment.
"""

from __future__ import annotations

import contextlib
import fcntl
import json
import os
import select
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA_BIN = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4048" / "aura")))
STUB_DELAY_S = float(os.environ.get("AURA_4054_STUB_DELAY_S", "1.25"))
BODY_PAD = int(os.environ.get("AURA_4054_BODY_PAD", "4096"))
N = int(os.environ.get("AURA_4054_N", "2"))
BATCHES = int(os.environ.get("AURA_4054_BATCHES", "2"))
BATCH_TIMEOUT_S = float(os.environ.get("AURA_4054_BATCH_TIMEOUT_S", "45"))
PING_TIMEOUT_S = 8.0


class SlowLargeHandler(BaseHTTPRequestHandler):
    hits = 0
    lock = threading.Lock()

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length:
            self.rfile.read(length)
        with self.lock:
            SlowLargeHandler.hits += 1
            n = SlowLargeHandler.hits
        time.sleep(STUB_DELAY_S)
        content = f"OK{n}-" + ("Z" * BODY_PAD)
        body = json.dumps(
            {
                "id": "stub-4054",
                "choices": [{"message": {"role": "assistant", "content": content}}],
                "ok": True,
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # silence
        return


def start_stub():
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), SlowLargeHandler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, port


def kill_soft_zombies():
    try:
        out = subprocess.check_output(["pgrep", "-af", "build_soft4048/aura"], text=True)
    except subprocess.CalledProcessError:
        return
    my_pid = os.getpid()
    for line in out.splitlines():
        if "check_soft_ready_http_post" in line:
            continue
        parts = line.split(None, 1)
        if not parts:
            continue
        try:
            pid = int(parts[0])
        except ValueError:
            continue
        if pid == my_pid:
            continue
        if "serve-async" in line:
            with contextlib.suppress(OSError):
                os.kill(pid, signal.SIGKILL)


def json_exec_line(code: str) -> str:
    return json.dumps({"cmd": "exec", "code": code}, separators=(",", ":")) + "\n"


def read_status(proc: subprocess.Popen, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    buf = ""
    fd = proc.stdout.fileno()
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            err = ""
            try:
                os.set_blocking(proc.stderr.fileno(), False)
                err = proc.stderr.read() or ""
            except Exception:
                pass
            raise RuntimeError(f"aura exited rc={proc.returncode} stderr={err[-800:]!r}")
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not chunk:
            time.sleep(0.01)
            continue
        buf += chunk.decode("utf-8", errors="replace")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if not line or '"status"' not in line:
                continue
            i = line.find("{")
            j = line.rfind("}")
            if i < 0 or j <= i:
                continue
            try:
                return json.loads(line[i : j + 1])
            except json.JSONDecodeError:
                continue
    raise TimeoutError(f"no status within {timeout_s}s; buf_tail={buf[-400]!r}")


def send_exec(proc: subprocess.Popen, code: str, timeout_s: float) -> dict:
    proc.stdin.write(json_exec_line(code))
    proc.stdin.flush()
    return read_status(proc, timeout_s)


def batch_code(stub_url: str, n: int) -> str:
    # Modest request body; stub ignores it. Response is large (BODY_PAD).
    body = '{"model":"stub-4054","messages":[{"role":"user","content":"hi-' + ("q" * 64) + '"}]}'
    body_esc = body.replace("\\", "\\\\").replace('"', '\\"')
    binds = " ".join(
        f'(f{i} (fiber:spawn (lambda () (base64-encode (http-post "{stub_url}" "{body_esc}")))))' for i in range(n)
    )
    joins = " ".join(f"(fiber:join f{i})" for i in range(n))
    return f"(let ({binds}) (list {joins}))"


def main() -> int:
    if not AURA_BIN.is_file():
        print(f"SKIP: {AURA_BIN} missing — build the Soft tree to enable the two-batch check", file=sys.stderr)
        return 0
    lock_fd = os.open("/tmp/aura-soft-encoding-check.lock", os.O_CREAT | os.O_RDWR)
    try:
        fcntl.lockf(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        print("SKIP: another Soft encoding check holds the lock", file=sys.stderr)
        return 0

    kill_soft_zombies()
    time.sleep(0.2)

    httpd, port = start_stub()
    stub_url = f"http://127.0.0.1:{port}/chat/completions"
    print(f"stub_url={stub_url} delay_s={STUB_DELAY_S} pad={BODY_PAD} N={N} batches={BATCHES}")
    print(f"aura_bin={AURA_BIN}")

    env = {**os.environ, "AURA_SANDBOX": "off", "AURA_PIPELINE_STRICT": "0"}
    proc = subprocess.Popen(
        [str(AURA_BIN), "--serve-async"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        bufsize=0,
    )
    try:
        time.sleep(0.4)
        if proc.poll() is not None:
            err = proc.stderr.read() if proc.stderr else ""
            print(f"FAIL: aura died at boot rc={proc.returncode} err={err[-1000]!r}")
            return 1

        warm = send_exec(proc, "(+ 1 2)", timeout_s=10.0)
        print(f"warm status={warm.get('status')} value={warm.get('value')}")
        if warm.get("status") != "ok":
            print("FAIL: warm exec", warm)
            return 1

        req = send_exec(
            proc,
            '(begin (require "std/llm" all:) (require "std/encoding" all:) '
            "(and (procedure? http-post) (procedure? base64-encode)))",
            timeout_s=15.0,
        )
        print(f"require status={req.get('status')} value={req.get('value')}")
        if req.get("status") != "ok" or str(req.get("value")) != "#t":
            print("FAIL: http-post/base64 not installed", req)
            return 1

        code = batch_code(stub_url, N)
        for bi in range(1, BATCHES + 1):
            t0 = time.monotonic()
            try:
                batch = send_exec(proc, code, timeout_s=BATCH_TIMEOUT_S)
            except Exception as exc:
                wall = int((time.monotonic() - t0) * 1000)
                print(f"FAIL: batch{bi} raised {type(exc).__name__}: {exc} wall_ms={wall}")
                try:
                    ping = send_exec(proc, "(+ 1 1)", timeout_s=PING_TIMEOUT_S)
                    print(f"post_fail_ping status={ping.get('status')} value={ping.get('value')}")
                except Exception as ping_exc:
                    print(f"post_fail_ping also failed: {ping_exc}")
                return 1
            wall = int((time.monotonic() - t0) * 1000)
            print(
                f"batch{bi} status={batch.get('status')} wall_ms={wall} value_len={len(str(batch.get('value') or ''))}"
            )
            if batch.get("status") != "ok":
                print("FAIL: denseness batch", batch)
                return 1
            # Intervening ping — must stay live after each batch (#4054).
            ping = send_exec(proc, "(+ 7 7)", timeout_s=PING_TIMEOUT_S)
            print(f"inter_ping{bi} status={ping.get('status')} value={ping.get('value')}")
            if ping.get("status") != "ok" or str(ping.get("value")) != "14":
                print("FAIL: inter-batch ping", ping)
                return 1

        post = send_exec(proc, "(+ 1 1)", timeout_s=PING_TIMEOUT_S)
        print(f"post_ping status={post.get('status')} value={post.get('value')}")
        if post.get("status") != "ok" or str(post.get("value")) != "2":
            print("FAIL: post-batch ping", post)
            return 1

        try:
            os.set_blocking(proc.stderr.fileno(), False)
            err_all = proc.stderr.read() or ""
        except Exception:
            err_all = ""
        print(f"stderr_snip={err_all[:500]!r}")
        if "Soft Ready profile" not in err_all:
            print("FAIL: banner missing Soft Ready honesty")
            return 1
        if (
            "production Ready" in err_all
            and "Do not stamp production Ready" not in err_all
            and "Soft Ready profile" not in err_all
        ):
            print("FAIL: banner claims production Ready")
            return 1

        print(
            f"PASS: Soft Ready two-batch denseness http-post #4054 "
            f"N={N} batches={BATCHES} delay_s={STUB_DELAY_S} pad={BODY_PAD}"
        )
        return 0
    finally:
        try:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=3)
        except Exception:
            pass
        with contextlib.suppress(Exception):
            httpd.shutdown()
        kill_soft_zombies()


if __name__ == "__main__":
    sys.exit(main())
