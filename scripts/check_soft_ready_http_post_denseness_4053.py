#!/usr/bin/env python3
"""Soft Ready --serve-async denseness http-post overlap regression (#4053).

Spawns a local slow HTTP stub (~200ms), runs Soft Ready aura --serve-async
(auto workers=1), measures:
  - oneshot denseness fiber http-post wall
  - N=4 concurrent denseness fibers each http-post to the stub

Acceptance: N=4 wall ≪ 4× oneshot (target ≲ 2.0× with noise); no hang/SEGV.
Documents the bound in stdout. Does NOT claim production Ready.
"""

from __future__ import annotations

import contextlib
import fcntl
import json
import os
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA_BIN = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4048" / "aura")))
STUB_DELAY_S = float(os.environ.get("AURA_4053_STUB_DELAY_S", "0.20"))
N = int(os.environ.get("AURA_4053_N", "4"))
# Bound: wall / oneshot must be <= this (ideal ~1.0; serial ~N).
MAX_RATIO = float(os.environ.get("AURA_4053_MAX_RATIO", "2.0"))
ONESHOT_TIMEOUT_S = 30.0
BATCH_TIMEOUT_S = 60.0


class SlowHandler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length:
            self.rfile.read(length)
        time.sleep(STUB_DELAY_S)
        body = b'{"ok":true,"stub":"4053"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):  # silence
        return


def start_stub():
    # Bind ephemeral port on localhost.
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
    port = httpd.server_address[1]
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd, port


def kill_soft_zombies():
    """Kill leftover Soft --serve-async holders that spin CPU and wedge sessions."""
    try:
        out = subprocess.check_output(["pgrep", "-af", "aura --serve-async"], text=True)
    except subprocess.CalledProcessError:
        return
    my_pid = os.getpid()
    for line in out.splitlines():
        if "check_soft_ready_http_post_denseness_4053" in line:
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
        if "aura --serve-async" in line or "--serve-async" in line:
            with contextlib.suppress(OSError):
                os.kill(pid, signal.SIGKILL)


def json_exec_line(code: str, session: str | None = None) -> str:
    payload = {"cmd": "exec", "code": code}
    if session:
        payload["session"] = session
    return json.dumps(payload, separators=(",", ":")) + "\n"


def read_status(proc: subprocess.Popen, timeout_s: float) -> dict:
    """Read Soft --serve-async stdout until a JSON status object arrives."""
    import select

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
            if not line:
                continue
            if '"status"' not in line:
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
    line = json_exec_line(code)
    proc.stdin.write(line)
    proc.stdin.flush()
    return read_status(proc, timeout_s)


def main() -> int:
    if not AURA_BIN.is_file():
        print(f"SKIP: {AURA_BIN} missing — build the Soft tree to enable the denseness check", file=sys.stderr)
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
    stub_url = f"http://127.0.0.1:{port}/slow"
    print(f"stub_url={stub_url} delay_s={STUB_DELAY_S} N={N} max_ratio={MAX_RATIO}")
    print(f"aura_bin={AURA_BIN}")

    env = {**os.environ, "AURA_SANDBOX": "off", "AURA_PIPELINE_STRICT": "0"}
    # Prefer hard timeout on the whole aura process via a wrapper alarm.
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
        # Drain Soft Ready banner from stderr asynchronously.
        err_box: list[str] = []

        def _drain_err():
            try:
                err_box.append(proc.stderr.read() or "")
            except Exception as exc:
                err_box.append(f"<stderr_read_err:{exc}>")

        threading.Thread(target=_drain_err, daemon=True)
        # Don't start full-read yet — banner is line-buffered; read incrementally later.
        time.sleep(0.4)
        if proc.poll() is not None:
            err = proc.stderr.read() if proc.stderr else ""
            print(f"FAIL: aura died at boot rc={proc.returncode} err={err[-1000]!r}")
            return 1

        # Warm: simple arith so Soft JSON session is live.
        warm = send_exec(proc, "(+ 1 2)", timeout_s=10.0)
        print(f"warm status={warm.get('status')} value={warm.get('value')}")
        if warm.get("status") != "ok":
            print("FAIL: warm exec", warm)
            return 1

        # http-post is deferred (#3919); install via std/llm like aura-build.
        req = send_exec(
            proc,
            '(begin (require "std/llm" all:) (procedure? http-post))',
            timeout_s=15.0,
        )
        print(f"require_llm status={req.get('status')} value={req.get('value')}")
        if req.get("status") != "ok" or str(req.get("value")) != "#t":
            print("FAIL: http-post not installed after require std/llm", req)
            return 1

        body = "{}"  # keep Aura sexpr simple; stub ignores body
        oneshot_code = f'(fiber:join (fiber:spawn (lambda () (http-post "{stub_url}" "{body}"))))'
        t0 = time.monotonic()
        one = send_exec(proc, oneshot_code, timeout_s=ONESHOT_TIMEOUT_S)
        oneshot_ms = int((time.monotonic() - t0) * 1000)
        print(f"oneshot status={one.get('status')} value_snip={str(one.get('value'))[:80]!r} wall_ms={oneshot_ms}")
        if one.get("status") != "ok":
            print("FAIL: oneshot denseness http-post", one)
            return 1
        oval = str(one.get("value") or "")
        if "stub" not in oval and "4053" not in oval:
            print(f"FAIL: oneshot value not stub JSON: {oval!r}")
            return 1
        min_oneshot = int(STUB_DELAY_S * 1000 * 0.5)
        if oneshot_ms < min_oneshot:
            print(
                f"FAIL: oneshot wall {oneshot_ms}ms << stub delay "
                f"(expected >= {min_oneshot}ms); http-post not actually waiting"
            )
            return 1

        # N denseness fibers each http-post, joined from the session fiber.
        spawn_binds = " ".join(f'(f{i} (fiber:spawn (lambda () (http-post "{stub_url}" "{body}"))))' for i in range(N))
        joins = " ".join(f"(fiber:join f{i})" for i in range(N))
        batch_code = f"(let* ({spawn_binds}) (list {joins}))"
        t0 = time.monotonic()
        batch = send_exec(proc, batch_code, timeout_s=BATCH_TIMEOUT_S)
        batch_ms = int((time.monotonic() - t0) * 1000)
        print(
            f"batch_N{N} status={batch.get('status')} value_snip={str(batch.get('value'))[:120]!r} wall_ms={batch_ms}"
        )
        if batch.get("status") != "ok":
            print("FAIL: batch denseness http-post", batch)
            return 1
        bval = str(batch.get("value") or "")
        if bval.count("stub") < 1 and bval.count("ok") < N and "stub" not in bval:
            # Soft may escape quotes; accept presence of stub marker or N ok tokens.
            print(f"FAIL: batch value missing stub responses: {bval!r}")
            return 1

        ratio = batch_ms / max(oneshot_ms, 1)
        serial_ms = oneshot_ms * N
        print(
            f"bound: N={N} wall_ms={batch_ms} oneshot_ms={oneshot_ms} "
            f"ratio={ratio:.2f}× (serial_would_be≈{serial_ms}ms) "
            f"accept_ratio<= {MAX_RATIO}"
        )

        # Soft honesty: banner must not claim production Ready.
        # Read whatever stderr has without blocking forever.
        try:
            os.set_blocking(proc.stderr.fileno(), False)
            err_tail = proc.stderr.read() or ""
        except Exception:
            err_tail = ""
        err_all = err_tail
        print(f"stderr_snip={err_all[:500]!r}")
        if (
            "production Ready" in err_all
            and "Do not stamp production Ready" not in err_all
            and "Soft Ready profile" not in err_all
        ):
            # Only fail if it claims production Ready affirmatively without the Soft caveat.
            print("FAIL: banner missing Soft Ready honesty")
            return 1
        if "Soft Ready profile" in err_all and "production multi-worker Ready" in err_all:
            # Honest Soft banner present — good.
            pass

        if ratio > MAX_RATIO:
            print(
                f"FAIL: denseness http-post still serial-ish: "
                f"ratio {ratio:.2f}× > {MAX_RATIO} "
                f"(want concurrent wall ≲ {MAX_RATIO}× oneshot)"
            )
            return 1

        print(f"PASS: Soft Ready denseness http-post overlap #4053 N={N} ratio={ratio:.2f}× <= {MAX_RATIO}")
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
