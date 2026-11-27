#!/usr/bin/env python3
"""Issue #5136: the sandbox preset applies to lib/std modules too.

A stdlib module that needs an effect must be refused by the default
(production) face with a catchable error, not by an abort, and the same
require must succeed once the sandbox is explicitly off. Before this test
the property was only implied by the cross-mode corpus, which pins
stdout/rc parity but never asserts the denial itself.
"""

from __future__ import annotations

import http.server
import os
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

# Issue #5447: an empty store keeps the "no key" cases offline even when the
# operator configured $HOME/.aura/secrets.
_EMPTY_SECRET_STORE = tempfile.mkdtemp(prefix="aura-llm-empty-secrets-")

PROGRAM = """(display (try (begin (require "std/ffi" all:) (quote ok))
                        (catch (e) (quote denied))))
(newline)
"""


def run(face_env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("AURA_")}
    env.update(face_env)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as fh:
        fh.write(PROGRAM)
        path = fh.name
    return subprocess.run(
        [str(AURA_BIN), "--load", path],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
        cwd=str(ROOT),
    )


LLM_CHAT = """(require "std/llm" all:)
(define r (llm:chat "sys" "ping"))
(display "ok=")
(display (hash-ref r "ok" 'missing))
(newline)
(display "error=")
(display (hash-ref r "error" 'missing))
(newline)
"""

NETWORK_GRANT = (
    "sandbox has not granted network: pass --grant net or --grant network, "
    "or set AURA_SANDBOX=off in a controlled environment"
)


def run_llm_program(
    program: str,
    args: tuple[str, ...],
    extra: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("AURA_")}
    for key in ("LLM_API_KEY", "LLM_BASE_URL", "LLM_MODEL", "LLM_TIMEOUT"):
        env.pop(key, None)
    env["AURA_SECRET_STORE"] = _EMPTY_SECRET_STORE
    env["AURA_PATH"] = str(ROOT / "lib")
    if extra:
        env.update(extra)
    with tempfile.NamedTemporaryFile("w", suffix=".aura", delete=False) as fh:
        fh.write(program)
        path = fh.name
    return subprocess.run(
        [str(AURA_BIN), *args, path],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
        cwd=str(ROOT),
    )


def run_llm_chat(*args: str, **extra: str) -> subprocess.CompletedProcess[str]:
    return run_llm_program(LLM_CHAT, args, extra)


_STATS_HEAD = """(require "std/llm" all:)
(define (show)
  (let ((s (llm:call-stats)))
    (display (list "success" (hash-ref s "success")
                   "fail" (hash-ref s "fail")
                   "rate-limited" (hash-ref s "rate-limited")
                   "no-key" (hash-ref s "no-key")))
    (newline)))
(llm:call-stats-reset!)
(llm:rate-limit-set! 0)
"""


def _stats_program(body: str) -> str:
    return _STATS_HEAD + body + "\n(show)\n"


def _counter(stdout: str, name: str) -> int:
    match = re.search(rf"(?:^|[ (]){re.escape(name)} (-?\d+)", stdout)
    if match is None:
        raise AssertionError(stdout)
    return int(match.group(1))


class _ChatReply(http.server.BaseHTTPRequestHandler):
    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0") or 0)
        if length:
            self.rfile.read(length)
        payload = (
            b'{"choices":[{"message":{"content":"pong"}}],'
            b'"usage":{"total_tokens":12,"prompt_tokens":9,"completion_tokens":3,'
            b'"completion_tokens_details":{"reasoning_tokens":0},'
            b'"prompt_tokens_details":{"cached_tokens":4}}}'
        )
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt: str, *args) -> None:
        return


class StdSandboxPreset(unittest.TestCase):
    def test_default_face_denies_and_stays_catchable(self) -> None:
        r = run({"AURA_PATH": str(ROOT / "lib")})
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("denied", r.stdout)
        self.assertNotIn("CRASH", r.stderr)
        self.assertNotIn("SIGABRT", r.stderr)

    def test_sandbox_off_allows_the_same_require(self) -> None:
        r = run({"AURA_PATH": str(ROOT / "lib"), "AURA_SANDBOX": "off"})
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("ok", r.stdout)


class LlmChatNetworkGrant(unittest.TestCase):
    """Issue #5287: llm:chat names the network grant when http-post is absent."""

    def test_missing_network_names_the_grant(self) -> None:
        faces = (
            ("--quiet",),
            ("--quiet", "--preset", "prod"),
            ("--quiet", "--deny", "network"),
        )
        for args in faces:
            r = run_llm_chat(*args)
            self.assertEqual(r.returncode, 0, args + (r.stdout, r.stderr[-800:]))
            self.assertIn("ok=#f", r.stdout)
            self.assertIn("error=" + NETWORK_GRANT, r.stdout)
            self.assertNotIn("error=no-http-post", r.stdout)
            self.assertNotIn("http-post:", r.stdout)

    def test_grant_or_sandbox_off_does_not_ask_for_network(self) -> None:
        faces = (
            (("--quiet", "--grant", "net"), {}),
            (("--quiet", "--grant", "network"), {}),
            (("--quiet",), {"AURA_SANDBOX": "off"}),
        )
        for args, extra in faces:
            r = run_llm_chat(*args, **extra)
            self.assertEqual(r.returncode, 0, args + (r.stdout, r.stderr[-800:]))
            self.assertIn("ok=#f", r.stdout)
            self.assertIn("error=empty", r.stdout)
            self.assertNotIn("sandbox has not granted network", r.stdout)


class LlmCallStats(unittest.TestCase):
    """Issue #5293: success, fail, rate-limited, and no-key move."""

    def test_no_key_bumps_no_key(self) -> None:
        program = _stats_program(
            '(define r (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
            '(display "ok=") (display (hash-ref r "ok"))\n'
            '(display " error=") (display (hash-ref r "error")) (newline)\n'
        )
        r = run_llm_program(program, ("--quiet", "--preset", "dev", "--grant", "net"))
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("ok=#f", r.stdout)
        self.assertIn("error=empty", r.stdout)
        self.assertEqual(_counter(r.stdout, "no-key"), 1)
        self.assertEqual(_counter(r.stdout, "fail"), 0)
        self.assertEqual(_counter(r.stdout, "rate-limited"), 0)
        self.assertEqual(_counter(r.stdout, "success"), 0)

    def test_rate_limit_bumps_rate_limited(self) -> None:
        program = _stats_program(
            "(llm:rate-limit-set! 1)\n"
            '(define r1 (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
            '(define r2 (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
            '(display "call1 error=") (display (hash-ref r1 "error")) (newline)\n'
            '(display "call2 error=") (display (hash-ref r2 "error")) (newline)\n'
            '(display "remaining=") (display (llm:rate-limit-remaining)) (newline)\n'
        )
        r = run_llm_program(program, ("--quiet", "--preset", "dev", "--grant", "net"))
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("call1 error=empty", r.stdout)
        self.assertIn("call2 error=rate-limited", r.stdout)
        self.assertIn("remaining=0", r.stdout)
        self.assertEqual(_counter(r.stdout, "no-key"), 1)
        self.assertEqual(_counter(r.stdout, "rate-limited"), 1)
        self.assertEqual(_counter(r.stdout, "fail"), 0)
        self.assertEqual(_counter(r.stdout, "success"), 0)

    def test_transport_failure_bumps_fail(self) -> None:
        program = _stats_program(
            '(define r (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
            '(display "ok=") (display (hash-ref r "ok"))\n'
            '(display " error=") (display (hash-ref r "error")) (newline)\n'
        )
        r = run_llm_program(
            program,
            ("--quiet", "--preset", "dev", "--grant", "net"),
            {"LLM_API_KEY": "x", "LLM_BASE_URL": "http://127.0.0.1:1", "LLM_MODEL": "dummy"},
        )
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("ok=#f", r.stdout)
        self.assertIn("error=http-post:", r.stdout)
        self.assertEqual(_counter(r.stdout, "fail"), 1)
        self.assertEqual(_counter(r.stdout, "no-key"), 0)
        self.assertEqual(_counter(r.stdout, "rate-limited"), 0)
        self.assertEqual(_counter(r.stdout, "success"), 0)

    def test_parsed_content_bumps_success(self) -> None:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _ChatReply)
        port = int(server.server_address[1])
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            program = _stats_program(
                '(define r (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
                '(display "ok=") (display (hash-ref r "ok"))\n'
                '(display " content=") (display (hash-ref r "content")) (newline)\n'
            )
            r = run_llm_program(
                program,
                ("--quiet", "--preset", "dev", "--grant", "net"),
                {
                    "LLM_API_KEY": "x",
                    "LLM_BASE_URL": f"http://127.0.0.1:{port}",
                    "LLM_MODEL": "dummy",
                },
            )
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("ok=#t", r.stdout)
        self.assertIn("content=pong", r.stdout)
        self.assertEqual(_counter(r.stdout, "success"), 1)
        self.assertEqual(_counter(r.stdout, "fail"), 0)
        self.assertEqual(_counter(r.stdout, "no-key"), 0)
        self.assertEqual(_counter(r.stdout, "rate-limited"), 0)

    def test_ungranted_network_leaves_outcome_counters_at_zero(self) -> None:
        program = _stats_program(
            '(define r (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
            '(display "error=") (display (hash-ref r "error")) (newline)\n'
        )
        r = run_llm_program(program, ("--quiet", "--preset", "dev", "--deny", "network"))
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("error=" + NETWORK_GRANT, r.stdout)
        self.assertEqual(_counter(r.stdout, "success"), 0)
        self.assertEqual(_counter(r.stdout, "fail"), 0)
        self.assertEqual(_counter(r.stdout, "rate-limited"), 0)
        self.assertEqual(_counter(r.stdout, "no-key"), 0)


class LlmTokenUsage(unittest.TestCase):
    """Issue #5481: both chat entry points expose the provider's usage."""

    def test_chat_and_vision_report_usage(self) -> None:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _ChatReply)
        port = int(server.server_address[1])
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            program = _stats_program(
                '(define r (llm:chat "sys" "ping" (hash "max_tokens" 8) 2))\n'
                '(define u (hash-ref r "usage" #f))\n'
                '(display "chat-usage=") (display (hash? u)) (newline)\n'
                '(display "prompt=") (display (hash-ref u "prompt_tokens" 0)) (newline)\n'
                '(display "completion=") (display (hash-ref u "completion_tokens" 0)) (newline)\n'
                '(display "total=") (display (hash-ref u "total_tokens" 0)) (newline)\n'
                '(display "reasoning=") (display (hash-ref u "reasoning_tokens" -1)) (newline)\n'
                '(display "cached=") (display (hash-ref u "cached_tokens" -1)) (newline)\n'
                '(define v (llm:chat-vision "sys" (list (llm:text-part "ping")) (hash "max_tokens" 8) 2))\n'
                '(define vu (hash-ref v "usage" #f))\n'
                '(display "vision-usage=") (display (hash? vu)) (newline)\n'
                '(display "vision-total=") (display (hash-ref vu "total_tokens" 0)) (newline)\n'
            )
            r = run_llm_program(
                program,
                ("--quiet", "--preset", "dev", "--grant", "net"),
                {
                    "LLM_API_KEY": "x",
                    "LLM_BASE_URL": f"http://127.0.0.1:{port}",
                    "LLM_MODEL": "dummy",
                },
            )
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(r.returncode, 0, r.stderr[-800:])
        self.assertIn("chat-usage=#t", r.stdout)
        self.assertIn("prompt=9", r.stdout)
        self.assertIn("completion=3", r.stdout)
        self.assertIn("total=12", r.stdout)
        self.assertIn("reasoning=0", r.stdout)
        self.assertIn("cached=4", r.stdout)
        self.assertIn("vision-usage=#t", r.stdout)
        self.assertIn("vision-total=12", r.stdout)


if __name__ == "__main__":
    unittest.main()
