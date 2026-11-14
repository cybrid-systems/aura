"""Regression for #5158: --serve and --serve-async share one line protocol.

A client that sends the documented command set must be accepted by both
modes, including a plain s-expression and session action list. Reply bytes
differ by the async session envelope; the accepted command set does not.
"""

from __future__ import annotations

import os
import re
import select
import subprocess
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
AURA = ROOT / "build" / "aura"

# Same list, same order, both modes. Accepted = a protocol status that is
# not an error. Rejected commands stay out of the set.
COMMANDS: list[tuple[str, str]] = [
    ("exec", '{"cmd":"exec","code":"(display 1)"}\n'),
    ("eval", '{"cmd":"eval","code":"(display 1)"}\n'),
    ("session-list", '{"cmd":"session","action":"list"}\n'),
    ("plain", "(display (+ 1 2))\n"),
    ("session-create", '{"cmd":"session","action":"create","name":"s1"}\n'),
    ("no-cmd", '{"code":"(+ 1 2)"}\n'),
    ("unknown", '{"cmd":"nope","code":"(display 1)"}\n'),
]

ACCEPT = {"ok", "created", "defined", "closure", "sent", "deleted"}


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        e.pop(k, None)
    e["AURA_RUNTIME_DIR"] = str(ROOT)
    return e


def _complete(blob: str) -> bool:
    if '"sessions"' in blob and '"active"' not in blob:
        return False
    return '"status"' in blob


class _Pipe:
    """Stdout replies, with a leftover buffer so one read can hold many lines.

    Sync flushes the whole session when the process exits, and session-list
    is more than one line. stderr is drained in the same select so a log
    line cannot fill the pipe and stall the server.
    """

    def __init__(self, proc: subprocess.Popen[bytes]) -> None:
        self.proc = proc
        self.buf = b""
        self.err = b""

    def read_reply(self, timeout: float) -> str:
        assert self.proc.stdout is not None
        out_fd = self.proc.stdout.fileno()
        err_fd = self.proc.stderr.fileno() if self.proc.stderr is not None else None
        deadline = time.monotonic() + timeout
        lines: list[str] = []
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                lines.append(line.decode(errors="replace") + "\n")
                blob = "".join(lines)
                if _complete(blob):
                    return blob
            if time.monotonic() >= deadline:
                break
            watch = [out_fd] if err_fd is None else [out_fd, err_fd]
            ready, _, _ = select.select(watch, [], [], min(deadline - time.monotonic(), 0.5))
            if not ready:
                if self.proc.poll() is not None and not self.buf:
                    break
                continue
            if err_fd is not None and err_fd in ready:
                chunk = os.read(err_fd, 65536)
                if chunk:
                    self.err += chunk
            if out_fd in ready:
                data = os.read(out_fd, 65536)
                if not data:
                    if not self.buf:
                        break
                    continue
                self.buf += data
        return "".join(lines) + self.buf.decode(errors="replace")


def drive(mode: str) -> tuple[list[tuple[str, str]], str]:
    proc = subprocess.Popen(
        [str(AURA), mode],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env(),
    )
    pipe = _Pipe(proc)
    replies: list[tuple[str, str]] = []
    try:
        assert proc.stdin is not None
        proc.stdin.write("".join(line for _, line in COMMANDS).encode())
        proc.stdin.close()
        for i, (name, _) in enumerate(COMMANDS):
            replies.append((name, pipe.read_reply(30 if i == 0 else 15)))
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        return replies, pipe.err.decode(errors="replace")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        if proc.stdin and not proc.stdin.closed:
            proc.stdin.close()
        if proc.stdout:
            proc.stdout.close()
        if proc.stderr:
            proc.stderr.close()


def status_of(blob: str) -> str | None:
    m = re.search(r'"status"\s*:\s*"([^"]*)"', blob)
    return m.group(1) if m else None


def msg_of(blob: str) -> str:
    m = re.search(r'"msg"\s*:\s*"((?:\\.|[^"\\])*)"', blob)
    return m.group(1) if m else ""


def accepted_set(replies: list[tuple[str, str]]) -> set[str]:
    return {name for name, blob in replies if status_of(blob) in ACCEPT}


def session_names(blob: str) -> set[str]:
    m = re.search(r'"sessions"\s*:\s*\[(.*?)\]', blob, re.S)
    if not m:
        return set()
    return set(re.findall(r'"([^"]*)"', m.group(1)))


class ServeProtocol5158(unittest.TestCase):
    def test_same_client_accepted_set(self) -> None:
        sync, sync_err = drive("--serve")
        async_, async_err = drive("--serve-async")
        sync_ok = accepted_set(sync)
        async_ok = accepted_set(async_)
        self.assertEqual(
            sync_ok,
            async_ok,
            f"accepted sets differ\nsync={sync!r}\nasync={async_!r}\nsync_err={sync_err!r}\nasync_err={async_err!r}",
        )
        self.assertEqual(
            sync_ok,
            {"exec", "session-list", "plain", "session-create"},
        )
        by_sync = dict(sync)
        by_async = dict(async_)
        self.assertIn("unknown command: eval", msg_of(by_sync["eval"]))
        self.assertIn("unknown command: eval", msg_of(by_async["eval"]))
        self.assertIn("unknown command: nope", msg_of(by_sync["unknown"]))
        self.assertIn("unknown command: nope", msg_of(by_async["unknown"]))
        self.assertIn("missing cmd field", msg_of(by_sync["no-cmd"]))
        self.assertIn("missing cmd field", msg_of(by_async["no-cmd"]))
        self.assertNotIn("missing cmd", msg_of(by_async["plain"]))
        self.assertNotIn("unknown action", msg_of(by_async["session-list"]))
        self.assertIn("default", session_names(by_sync["session-list"]))
        self.assertEqual(
            session_names(by_sync["session-list"]),
            session_names(by_async["session-list"]),
        )
        self.assertEqual(status_of(by_sync["plain"]), "ok")
        self.assertEqual(status_of(by_async["plain"]), "ok")
        self.assertEqual(status_of(by_sync["session-create"]), "created")
        self.assertEqual(status_of(by_async["session-create"]), "created")

    def test_help_all_documents_the_protocol(self) -> None:
        r = subprocess.run(
            [str(AURA), "help", "--all"],
            capture_output=True,
            text=True,
            env=env(),
            timeout=20,
        )
        self.assertEqual(r.returncode, 0, r.stderr)
        text = r.stdout
        for needle in (
            "one request per line",
            "s-expression",
            '{"cmd":"exec"',
            '{"cmd":"session","action":"list"',
            '{"cmd":"session","action":"create"',
            '{"status":"ok","value":',
            '{"session":"default","status":"ok"',
            "sessions",
            "session-send",
            "session-recv",
            "--serve-async",
            "docs/embedding.md",
        ):
            self.assertIn(needle, text, needle)
