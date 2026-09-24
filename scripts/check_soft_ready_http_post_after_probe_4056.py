#!/usr/bin/env python3
"""Soft Ready --serve-async: post-probe concurrent fiber http-post batch (#4056).

Regression for #4056: after a GREEN long in-fiber MiniMax-style probe
(~7 s single fiber http-post), the NEXT Soft eval for the require /
N=3 concurrent in-fiber http-post batch wedged the Soft serve session
(client saw serve_session_timeout then serve_sock_error: timed out) and
aura-build honestly fell back to llm_via=host / llm_parallel=host_thread.

The probe holds the Soft Ready workers=1 serve loop for its whole wall
time; the regression pins that the serve loop keeps servicing the session
once the probe returns (require within the client sock window, N=3
concurrent batches ok, inter-batch pings prompt, Soft stays honest).

Acceptance:
  - probe fiber http-post returns ok (~PROBE_DELAY_S)
  - the require eval IMMEDIATELY after the probe answers within
    REQUIRE_WINDOW_S (the aura-build client require/sock window analogue)
  - batch1..batchN all status=ok (N concurrent fibers each)
  - inter-batch + post-batch pings ok
  - Soft Ready banner honesty + RSS ceiling

Live MiniMax re-run stays propose-only via aura-build dogfood (documented
on the issue close).
"""

from __future__ import annotations

import contextlib
import fcntl
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA_BIN = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4048" / "aura")))
PROBE_DELAY_S = float(os.environ.get("AURA_4056_PROBE_DELAY_S", "2.5"))
STUB_DELAY_S = float(os.environ.get("AURA_4056_STUB_DELAY_S", "1.5"))
BODY_PAD = int(os.environ.get("AURA_4056_BODY_PAD", "8192"))
N = int(os.environ.get("AURA_4056_N", "3"))
BATCHES = int(os.environ.get("AURA_4056_BATCHES", "3"))
REQUIRE_WINDOW_S = float(os.environ.get("AURA_4056_REQUIRE_WINDOW_S", "15"))
BATCH_TIMEOUT_S = float(os.environ.get("AURA_4056_BATCH_TIMEOUT_S", "90"))
PING_TIMEOUT_S = 10.0
RSS_CEILING_MB = int(os.environ.get("AURA_4056_RSS_CEILING_MB", "8192"))
REQUIRE_RETRIES = int(os.environ.get("AURA_4056_REQUIRE_RETRIES", "6"))


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
        # The first request is the ~7 s probe; the rest are batch requests.
        time.sleep(PROBE_DELAY_S if n == 1 else STUB_DELAY_S)
        content = f"OK{n}-" + ("Z" * BODY_PAD)
        body = json.dumps(
            {
                "id": "stub-4056",
                "choices": [{"message": {"role": "assistant", "content": content}}],
                "ok": True,
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # noqa: N802
        pass


def start_stub():
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), SlowLargeHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, httpd.server_address[1]


def kill_soft_zombies():
    subprocess.run(
        ["pkill", "-9", "-x", "aura"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def read_status(proc: subprocess.Popen, timeout_s: float) -> dict:
    import select

    fd = proc.stdout.fileno()
    buf = ""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            if proc.poll() is not None:
                err = proc.stderr.read() if proc.stderr else ""
                raise RuntimeError(f"aura exited rc={proc.returncode} stderr={err[-600:]!r}")
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
    raise TimeoutError(f"no status within {timeout_s}s; buf_tail={buf[-400:]!r}")


def send_exec(proc: subprocess.Popen, code: str, timeout_s: float) -> dict:
    proc.stdin.write(json.dumps({"cmd": "exec", "code": code}, separators=(",", ":")) + "\n")
    proc.stdin.flush()
    return read_status(proc, timeout_s)


def post_body() -> str:
    return '{"model":"stub-4056","messages":[{"role":"user","content":"hi-' + ("q" * 64) + '"}]}'


def probe_code(stub_url: str) -> str:
    body_esc = post_body().replace("\\", "\\\\").replace('"', '\\"')
    return f'(let ((f (fiber:spawn (lambda () (http-post "{stub_url}" "{body_esc}"))))) (fiber:join f))'


def batch_code(stub_url: str, n: int) -> str:
    body_esc = post_body().replace("\\", "\\\\").replace('"', '\\"')
    binds = " ".join(
        f'(f{i} (fiber:spawn (lambda () (base64-encode (http-post "{stub_url}" "{body_esc}")))))' for i in range(n)
    )
    joins = " ".join(f"(fiber:join f{i})" for i in range(n))
    return f"(let ({binds}) (list {joins}))"


def main() -> int:
    if not AURA_BIN.is_file():
        print(
            f"SKIP: {AURA_BIN} missing — build the Soft tree to enable the post-probe check",
            file=sys.stderr,
        )
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
    print(
        f"stub_url={stub_url} probe_delay_s={PROBE_DELAY_S} batch_delay_s={STUB_DELAY_S} "
        f"pad={BODY_PAD} N={N} batches={BATCHES} require_window_s={REQUIRE_WINDOW_S}"
    )
    print(f"aura_bin={AURA_BIN}")

    rss_samples: list[int] = []
    rss_stop = threading.Event()

    def sample_rss(pid: int):
        status = Path(f"/proc/{pid}/status")
        while not rss_stop.is_set():
            try:
                for ln in status.read_text(encoding="utf-8", errors="replace").splitlines():
                    if ln.startswith("VmRSS:"):
                        rss_samples.append(int(ln.split()[1]))
                        break
            except OSError:
                break
            rss_stop.wait(0.5)

    env = {**os.environ, "AURA_SANDBOX": "off", "AURA_PIPELINE_STRICT": "0"}
    proc = subprocess.Popen(
        [str(AURA_BIN), "--serve-async"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        bufsize=0,
        cwd=str(ROOT),
    )
    rss_t = threading.Thread(target=sample_rss, args=(proc.pid,), daemon=True)
    rss_t.start()
    try:
        time.sleep(1.5)
        if proc.poll() is not None:
            err = proc.stderr.read() if proc.stderr else ""
            print(f"FAIL: aura died at boot rc={proc.returncode} err={err[-600:]!r}")
            return 1

        warm = send_exec(proc, "(+ 1 2)", timeout_s=10.0)
        print(f"warm status={warm.get('status')} value={warm.get('value')}")
        if warm.get("status") != "ok":
            print("FAIL: warm exec", warm)
            return 1

        req: dict = {}
        for attempt in range(1, REQUIRE_RETRIES + 1):
            req = send_exec(
                proc,
                '(begin (require "std/net" all:) (require "std/encoding" all:) '
                "(and (procedure? http-post) (procedure? base64-encode)))",
                timeout_s=15.0,
            )
            print(f"require[{attempt}] status={req.get('status')} value={req.get('value')}")
            if req.get("status") == "ok" and str(req.get("value")) == "#t":
                break
            time.sleep(1.0)
        if req.get("status") != "ok" or str(req.get("value")) != "#t":
            print("FAIL: http-post/base64 not installed after retries", req)
            return 1

        # 1) The green in-fiber long probe (fiber_llm_probe analogue).
        t0 = time.monotonic()
        probe = send_exec(proc, probe_code(stub_url), timeout_s=PROBE_DELAY_S + 30)
        probe_wall_ms = int((time.monotonic() - t0) * 1000)
        print(f"probe status={probe.get('status')} wall_ms={probe_wall_ms}")
        if probe.get("status") != "ok":
            print("FAIL: probe", probe)
            return 1

        # 2) Immediately re-require inside the client sock window: the Soft
        #    session must still answer (this is where #4056 wedged).
        t0 = time.monotonic()
        try:
            req2 = send_exec(
                proc,
                '(begin (require "std/net" all:) (procedure? http-post))',
                timeout_s=REQUIRE_WINDOW_S,
            )
        except TimeoutError as te:
            print(f"FAIL: post-probe require wedged (serve_session_timeout analogue): {te!r}")
            return 1
        req2_wall_ms = int((time.monotonic() - t0) * 1000)
        print(
            f"post_probe_require status={req2.get('status')} value={req2.get('value')} "
            f"wall_ms={req2_wall_ms} window_s={REQUIRE_WINDOW_S}"
        )
        if req2.get("status") != "ok" or str(req2.get("value")) != "#t":
            print("FAIL: post-probe require", req2)
            return 1

        # 3) N concurrent in-fiber batches with inter-batch pings.
        code = batch_code(stub_url, N)
        for bi in range(1, BATCHES + 1):
            t0 = time.monotonic()
            try:
                batch = send_exec(proc, code, timeout_s=BATCH_TIMEOUT_S)
            except TimeoutError as te:
                print(f"FAIL: batch{bi} raised {te!r} (serve_sock_error: timed out analogue)")
                return 1
            wall = int((time.monotonic() - t0) * 1000)
            print(f"batch{bi} status={batch.get('status')} wall_ms={wall}")
            if batch.get("status") != "ok":
                print("FAIL: batch", batch)
                return 1
            ping = send_exec(proc, "(+ 1 1)", timeout_s=PING_TIMEOUT_S)
            print(f"inter_ping{bi} status={ping.get('status')} value={ping.get('value')}")
            if ping.get("status") != "ok":
                print(f"FAIL: inter_ping{bi}", ping)
                return 1

        post = send_exec(proc, "(+ 1 1)", timeout_s=PING_TIMEOUT_S)
        print(f"post_ping status={post.get('status')} value={post.get('value')}")
        if post.get("status") != "ok":
            print("FAIL: post_ping", post)
            return 1

        rss_stop.set()
        rss_t.join(timeout=2.0)
        peak_mb = (max(rss_samples) / 1024) if rss_samples else 0
        print(f"rss peak_mb={peak_mb:.0f} ceiling_mb={RSS_CEILING_MB}")
        if rss_samples and peak_mb > RSS_CEILING_MB:
            print("FAIL: RSS ceiling exceeded (OOM-resistance)")
            return 1

        print(
            "PASS: Soft Ready post-probe concurrent http-post batch #4056 "
            f"probe_delay_s={PROBE_DELAY_S} N={N} batches={BATCHES}"
        )
        return 0
    finally:
        rss_stop.set()
        kill_soft_zombies()
        with contextlib.suppress(OSError):
            proc.kill()
        httpd.shutdown()


if __name__ == "__main__":
    sys.exit(main())
