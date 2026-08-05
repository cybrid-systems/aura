"""Aura Repl host for multi-agent-search (budget gate domain).

Uses a PTY so the CLI flushes prompts/results (pipe mode fully-buffers stdout
on modern builds and starves line-oriented drivers).
"""

from __future__ import annotations

import contextlib
import os
import pty
import re
import select
import subprocess
import time
from pathlib import Path

from common import GATE_CASES, SEED_CODE

REPO = Path(__file__).resolve().parent.parent.parent
AURA_BIN = (REPO / "build" / "aura").resolve()


def esc(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class AuraRepl:
    """Interactive Aura driven over a PTY (echo + result + `> ` prompt)."""

    def __init__(self) -> None:
        if not AURA_BIN.exists():
            raise FileNotFoundError(f"Aura binary not found: {AURA_BIN} (run ./build.py build)")
        env = os.environ.copy()
        env.setdefault("AURA_PIPELINE_STRICT", "0")
        self._master, slave = pty.openpty()
        self.proc = subprocess.Popen(
            [str(AURA_BIN)],
            stdin=slave,
            stdout=slave,
            stderr=subprocess.STDOUT,
            env=env,
            close_fds=True,
        )
        os.close(slave)
        self._buf = ""
        # Drain banner until first prompt.
        self._read_until_prompt(timeout_s=15.0)
        self._buf = ""

    def _read_more(self, timeout_s: float = 0.3) -> None:
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            r, _, _ = select.select([self._master], [], [], 0.15)
            if not r:
                continue
            try:
                chunk = os.read(self._master, 8192)
            except OSError:
                break
            if not chunk:
                break
            self._buf += chunk.decode("utf-8", "replace")

    def _read_until_prompt(self, timeout_s: float = 30.0) -> str:
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            self._read_more(0.35)
            text = self._buf.replace("\r\n", "\n").replace("\r", "\n")
            if text.rstrip().endswith(">") or text.endswith("> "):
                return text
        return self._buf.replace("\r\n", "\n").replace("\r", "\n")

    def eval(self, expr: str, timeout_s: float = 45.0) -> str:
        self._buf = ""
        os.write(self._master, (expr + "\n").encode())
        text = self._read_until_prompt(timeout_s=timeout_s)
        lines = [ln for ln in text.split("\n") if ln.strip() != ""]
        # Drop pure prompt lines and input echo.
        cleaned: list[str] = []
        for ln in lines:
            s = ln.strip()
            if s in (">",):
                continue
            if s.startswith("> "):
                s = s[2:].strip()
            cleaned.append(s)
        if cleaned and cleaned[0] == expr.strip():
            cleaned = cleaned[1:]
        # Prefer last non-warning line as value.
        values = [ln for ln in cleaned if not ln.startswith("⚠") and not ln.startswith("warning")]
        if not values:
            if cleaned:
                return cleaned[-1]
            raise TimeoutError(f"Aura Repl timeout/no result for: {expr[:120]!r}")
        return values[-1]

    def close(self) -> None:
        if self.proc.poll() is not None:
            with contextlib.suppress(OSError):
                os.close(self._master)
            return
        try:
            os.write(self._master, b"(exit)\n")
            time.sleep(0.2)
        except OSError:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            with contextlib.suppress(Exception):
                self.proc.kill()
        with contextlib.suppress(OSError):
            os.close(self._master)


def install(repl: AuraRepl, code: str) -> None:
    r = repl.eval(f"(set-code {esc(code)})")
    if r.startswith("<error") or r.lower().startswith("error"):
        raise RuntimeError(f"set-code failed: {r}")
    er = repl.eval("(eval-current)")
    # define-only programs return #<procedure> with a warning — OK.
    bad = er.startswith("<error") or (er.lower().startswith("error") and "procedure" not in er.lower())
    if bad and "procedure" not in er and "⚠" not in er:
        raise RuntimeError(f"eval-current failed: {er}")


def snapshot(repl: AuraRepl, name: str = "mas") -> str:
    return repl.eval(f'(ast:snapshot "{name}")').strip()


def restore(repl: AuraRepl, snap: str, name: str = "mas") -> None:
    try:
        sid = int(re.sub(r"[^0-9-]", "", snap) or "0")
        if sid != 0:
            repl.eval(f"(ast:restore {sid})")
        else:
            repl.eval(f'(ast:restore "{name}")')
    except Exception:
        repl.eval(f'(ast:restore "{name}")')
    repl.eval("(eval-current)")


def node_count(repl: AuraRepl) -> float:
    raw = repl.eval(
        "(let ((r (query:root))) (if (not r) 0 (let cnt ((id r)) (+ 1 (apply + (map cnt (query :children id)))))))"
    ).strip()
    try:
        n = float(raw.split(";")[0].strip())
        return min(n, 10_000.0)
    except Exception:
        return 0.0


def _truthy_admit(raw: str) -> bool | None:
    s = raw.strip().split(";")[0].strip().lower()
    if s in ("#f", "false", "0", "()"):
        return False
    if s in ("#t", "true", "1"):
        return True
    if s.startswith("<error") or s.startswith("error"):
        return None
    if s == "" or "procedure" in s:
        return None
    return True


def gate_eval(repl: AuraRepl, x: int) -> bool | None:
    return _truthy_admit(repl.eval(f"(gate {x})"))


def fail_count(repl: AuraRepl) -> int:
    n = 0
    for x, want_admit in GATE_CASES:
        got = gate_eval(repl, x)
        if got is None or got != want_admit:
            n += 1
    return n


def measure(repl: AuraRepl, energy: float = 0.0) -> tuple[int, float, float]:
    from common import V_value

    fc, nc = fail_count(repl), node_count(repl)
    return fc, nc, V_value(fc, nc, energy)


def install_seed(repl: AuraRepl, code: str = SEED_CODE) -> None:
    install(repl, code)


def fiber_local_fanout_probe(repl: AuraRepl, n: int = 3) -> str:
    expr = (
        "(let ((ids (list"
        + "".join(f" (fiber:spawn (lambda () {i}))" for i in range(1, n + 1))
        + "))) (apply + (map fiber:join ids)))"
    )
    return repl.eval(expr)
