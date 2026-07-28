"""Shared Aura Repl driver for lyapunov-fact demos."""

from __future__ import annotations

import contextlib
import os
import select
import subprocess
import time
from pathlib import Path

from common import TEST_VALUE

REPO = Path(__file__).resolve().parent.parent.parent
AURA_BIN = (REPO / "build" / "aura").resolve()


def esc(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class AuraRepl:
    def __init__(self) -> None:
        if not AURA_BIN.exists():
            raise FileNotFoundError(f"Aura binary not found: {AURA_BIN}")
        env = os.environ.copy()
        env.setdefault("AURA_PIPELINE_STRICT", "0")
        self.proc = subprocess.Popen(
            [str(AURA_BIN)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )

    def eval(self, expr: str, timeout_s: float = 30.0) -> str:
        assert self.proc.stdin and self.proc.stdout and self.proc.stderr
        self.proc.stdin.write(expr + "\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            r, _, _ = select.select([self.proc.stdout, self.proc.stderr], [], [], 0.5)
            if not r:
                continue
            if self.proc.stderr in r:
                _ = self.proc.stderr.readline()  # drain warnings
            if self.proc.stdout in r:
                return self.proc.stdout.readline().rstrip("\n")
        raise TimeoutError(f"Aura Repl timeout for: {expr[:120]!r}")

    def close(self) -> None:
        if self.proc.poll() is not None:
            return
        try:
            assert self.proc.stdin
            self.proc.stdin.write("(quit)\n")
            self.proc.stdin.flush()
            self.proc.stdin.close()
            self.proc.wait(timeout=3)
        except Exception:
            with contextlib.suppress(Exception):
                self.proc.kill()


def install(repl: AuraRepl, code: str) -> None:
    r = repl.eval(f"(set-code {esc(code)})")
    if r.startswith("<error") or "error:" in r:
        raise RuntimeError(f"set-code failed: {r}")
    repl.eval("(eval-current)")


def fact_n(repl: AuraRepl, n: int = 10) -> int | None:
    raw = repl.eval(f"(fact {n})").strip()
    try:
        return int(raw.split(";")[0].strip())
    except Exception:
        return None


def test_fail(repl: AuraRepl) -> int:
    return 0 if fact_n(repl, 10) == TEST_VALUE else 1


def recursive_residual(repl: AuraRepl) -> int:
    raw = repl.eval('(length (query:calls "fact"))').strip()
    try:
        return int(raw)
    except Exception:
        return 0


def node_count(repl: AuraRepl) -> float:
    raw = repl.eval(
        "(let ((r (query:root))) (if (not r) 0 (let cnt ((id r)) (+ 1 (apply + (map cnt (query:children id)))))))"
    ).strip()
    try:
        return float(raw)
    except Exception:
        return 0.0


def snapshot(repl: AuraRepl, name: str = "try") -> str:
    return repl.eval(f'(ast:snapshot "{name}")').strip()


def restore(repl: AuraRepl, snap: str, name: str = "try") -> None:
    try:
        sid = int(snap)
        repl.eval(f"(ast:restore {sid})")
    except Exception:
        repl.eval(f'(ast:restore "{name}")')
    repl.eval("(eval-current)")
