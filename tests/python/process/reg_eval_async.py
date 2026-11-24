#!/usr/bin/env python3
"""Issue #4880: --serve-async eval:async results.

Issue #4887 removed the legacy enqueue primitive; eval:async is the
supported path.


The old pool wiring treated the first eventfd wake (a stdin broadcast) as
completion, so both primitives returned an empty value, and the pool then
wrote the submitting fiber's eventfd after it was closed. This test drives
the issue's exact repro through --serve-async and checks the values plus
the absence of an EBADF write.
"""

from __future__ import annotations

import json
import os
import select
import subprocess
import sys
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

REQUESTS = [
    {"session": "default", "cmd": "exec", "code": '(eval:async "(+ 1 2)")'},
    {
        "session": "default",
        "cmd": "exec",
        "code": ('(eval:async "(begin (define (s n a) (if (= n 0) a (s (- n 1) (+ a 1)))) (s 3000000 0))")'),
    },
]


class TestIssue4880(unittest.TestCase):
    def test_serve_async_results(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")

        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env.pop("AURA_PIPELINE_STRICT", None)
        payload = "".join(json.dumps(r) + "\n" for r in REQUESTS)
        try:
            proc = subprocess.run(
                [str(AURA_BIN), "--serve-async"],
                input=payload,
                text=True,
                capture_output=True,
                timeout=30,
                env=env,
            )
        except subprocess.TimeoutExpired:
            self.fail("--serve-async did not finish the #4880 repro (hang)")

        rows = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                rows.append(json.loads(line))
        self.assertEqual(
            len(rows),
            len(REQUESTS),
            f"rows={rows!r} stdout={proc.stdout[-600:]!r} stderr={proc.stderr[-600:]!r}",
        )
        self.assertEqual(rows[0].get("status"), "ok")
        self.assertEqual(rows[0].get("value"), '"3"')
        self.assertEqual(rows[1].get("status"), "ok")
        self.assertEqual(rows[1].get("value"), '"3000000"')
        self.assertNotIn("Bad file descriptor", proc.stderr)


def _next_reply(proc: subprocess.Popen[bytes], buf: list[bytes], timeout: float) -> dict | None:
    """The next JSON reply line, or None at the deadline / EOF."""
    end = time.monotonic() + timeout
    while True:
        line, nl, rest = buf[0].partition(b"\n")
        if nl:
            buf[0] = rest
            text = line.strip()
            if text.startswith(b"{"):
                return json.loads(text)
            continue
        remaining = end - time.monotonic()
        if remaining <= 0:
            return None
        ready, _, _ = select.select([proc.stdout], [], [], remaining)
        if not ready:
            return None
        chunk = os.read(proc.stdout.fileno(), 65536)
        if not chunk:
            return None
        buf[0] += chunk


class TestTopLevelSleep5435(unittest.TestCase):
    """Issue #5435: a top-level (fiber:sleep) must not wedge the other sessions.

    A session exec keeps the commit lock across its own sleep (a second eval
    would rebind the workspace under the suspended one), so the other
    session's exec waits for it. That wait used to block the one Soft worker
    inside the stdout capture window: neither session ever answered.
    """

    def test_other_session_answers_after_top_level_sleep(self) -> None:
        if not AURA_BIN.exists():
            self.skipTest(f"{AURA_BIN} not built")
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        env.pop("AURA_PIPELINE_STRICT", None)
        proc = subprocess.Popen(
            [str(AURA_BIN), "--serve-async"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
            env=env,
        )
        buf = [b""]
        try:
            assert proc.stdin is not None
            proc.stdin.write(b'{"cmd":"session","action":"create","name":"b"}\n')
            proc.stdin.write(b'{"cmd":"exec","code":"(fiber:sleep 1500)"}\n')
            time.sleep(0.5)
            proc.stdin.write(b'{"cmd":"exec","session":"b","code":"(+ 1 2)"}\n')
            created = _next_reply(proc, buf, 5.0)
            self.assertEqual((created or {}).get("status"), "created")
            replies = []
            for _ in range(2):
                reply = _next_reply(proc, buf, 8.0)
                self.assertIsNotNone(
                    reply,
                    "a session got no reply: the exec waiting for the sleeping "
                    "session blocked the worker inside the capture window",
                )
                replies.append(reply)
            own = next((r for r in replies if r.get("session") == "default"), None)
            other = next((r for r in replies if r.get("session") == "b"), None)
            self.assertIsNotNone(own, f"replies={replies!r}")
            self.assertIsNotNone(other, f"replies={replies!r}")
            self.assertEqual(
                (own.get("status"), own.get("value"), own.get("display", "")),
                ("ok", "#t", ""),
                f"reply={own!r}",
            )
            self.assertEqual((other.get("status"), other.get("value")), ("ok", "3"), f"reply={other!r}")
        finally:
            if proc.stdin is not None:
                proc.stdin.close()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
