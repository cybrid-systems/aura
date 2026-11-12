"""Regression for #5155: aura lsp JSON-RPC parser has no nesting limit.

A ~120 KB message of brackets used to SIGSEGV the language server with an
empty stderr, because the parser recursed without a cap and `aura lsp`
returned before the crash handler was installed.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA = ROOT / "build" / "aura"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        e.pop(k, None)
    e["AURA_RUNTIME_DIR"] = str(ROOT)
    return e


def frame_raw(body: str) -> bytes:
    raw = body.encode()
    return f"Content-Length: {len(raw)}\r\n\r\n".encode() + raw


def frame(obj: dict) -> bytes:
    return frame_raw(json.dumps(obj))


def read_frames(raw: bytes) -> list[dict]:
    out = []
    while raw:
        head, _, rest = raw.partition(b"\r\n\r\n")
        if not head or b"Content-Length:" not in head:
            break
        n = int(head.split(b":")[1].strip())
        out.append(json.loads(rest[:n]))
        raw = rest[n:]
    return out


def nested_arrays(n: int) -> str:
    return "[" * n + "0" + "]" * n


def nested_objects(n: int) -> str:
    return '{"a":' * n + "1" + "}" * n


class LspDepth5155(unittest.TestCase):
    def test_deep_message_is_an_error_and_the_server_continues(self) -> None:
        deep_arr = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"x":' + nested_arrays(100000) + "}}"
        deep_obj = '{"jsonrpc":"2.0","id":2,"method":"initialize","params":{"x":' + nested_objects(100000) + "}}"
        ok = {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "initialize",
            "params": {"x": json.loads(nested_arrays(20))},
        }
        msgs = (
            frame_raw(deep_arr)
            + frame_raw(deep_obj)
            + frame(ok)
            + frame({"jsonrpc": "2.0", "id": 4, "method": "shutdown"})
            + frame({"jsonrpc": "2.0", "method": "exit"})
        )
        r = subprocess.run(
            [str(AURA), "lsp"],
            input=msgs,
            capture_output=True,
            env=env(),
            timeout=30,
        )
        err = r.stderr.decode(errors="replace")
        self.assertNotEqual(r.returncode, -signal.SIGSEGV, err)
        self.assertNotIn("SIGSEGV", err)
        self.assertNotIn("AURA CRASH", err)
        self.assertEqual(r.returncode, 0, err)
        frames = read_frames(r.stdout)
        by_id = {f.get("id"): f for f in frames}
        self.assertEqual(by_id[1]["error"]["code"], -32700)
        self.assertIn("nesting too deep", by_id[1]["error"]["message"])
        self.assertEqual(by_id[2]["error"]["code"], -32700)
        self.assertIn("nesting too deep", by_id[2]["error"]["message"])
        self.assertIn("definitionProvider", by_id[3]["result"]["capabilities"])
        self.assertIsNone(by_id[4].get("result"))

    def test_crash_handler_prints_diagnostics(self) -> None:
        p = subprocess.Popen(
            [str(AURA), "lsp"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env(),
        )
        assert p.stdin and p.stdout and p.stderr
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}))
        p.stdin.flush()
        header = b""
        while b"\r\n\r\n" not in header:
            chunk = p.stdout.read(1)
            self.assertTrue(chunk, "lsp closed before a reply")
            header += chunk
        n = int(header.split(b":")[1].strip())
        body = p.stdout.read(n)
        self.assertIn(b"aura-lsp", body)
        p.send_signal(signal.SIGSEGV)
        try:
            err = p.stderr.read()
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait(timeout=5)
            self.fail("aura lsp ignored SIGSEGV")
        finally:
            p.stdin.close()
            p.stdout.close()
            p.stderr.close()
        text = err.decode(errors="replace")
        self.assertIn("=== AURA CRASH:", text)
        self.assertIn("SIGSEGV", text)
        self.assertIn("signal 11", text)


if __name__ == "__main__":
    unittest.main()
