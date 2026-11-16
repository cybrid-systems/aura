#!/usr/bin/env python3
"""Issue #5240: --error-format=json emits one stable machine-readable diagnostic."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402


def run(args: list[str], cwd: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env["AURA_SANDBOX"] = "off"
    return subprocess.run([str(AURA_BIN), *args], cwd=cwd, capture_output=True, text=True, env=env, timeout=120)


def last_json(stderr: str) -> dict:
    lines = [ln for ln in stderr.splitlines() if ln.startswith("{")]
    assert lines, stderr
    return json.loads(lines[-1])


def frame(obj: dict) -> bytes:
    raw = json.dumps(obj).encode()
    return f"Content-Length: {len(raw)}\r\n\r\n".encode() + raw


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


class DiagJson(unittest.TestCase):
    def test_parse_error_has_stable_code_and_span(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            r = run(["--error-format=json", str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            obj = last_json(r.stderr)
            self.assertEqual(obj["code"], "ParseError")
            self.assertEqual(obj["file"], str(p))
            self.assertEqual(obj["span"]["start"], {"line": 1, "column": 10})
            self.assertIn("unterminated string literal", obj["message"])
            self.assertIn("abc", obj["snippet"])

    def test_runtime_error_code_and_position(self) -> None:
        r = run(["--error-format", "json", "-e", "(car 5)"])
        self.assertNotEqual(r.returncode, 0)
        obj = last_json(r.stderr)
        self.assertEqual(obj["code"], "TypeError")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})
        self.assertIn("car: not a pair", obj["message"])
        self.assertIsInstance(obj["code"], str)

    def test_refusal_code_is_effect_denied(self) -> None:
        # Issue #5241: the JSON code and the human prefix come from the
        # structured kind. Before, a denied set-car! printed "effect-denied"
        # on the text face while this JSON carried code TypeError. Every
        # refusal family creates its kind now, not just the mutate gate.
        cases = {
            "mutate": "(set-car! (cons 1 2) 9)\n",
            "write-file": '(write-file "x.txt" "y")\n',
            "exec": '(require "std/process" all:)\n(shell "echo hi")\n',
        }
        env = dict(os.environ)
        for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_GRANT"):
            env.pop(k, None)
        for name, src in cases.items():
            with self.subTest(family=name), tempfile.TemporaryDirectory() as d:
                p = Path(d) / "d.aura"
                p.write_text(src)
                r = subprocess.run(
                    [str(AURA_BIN), "--error-format=json", str(p)],
                    cwd=d,
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=120,
                )
                self.assertNotEqual(r.returncode, 0)
                obj = last_json(r.stderr)
                self.assertEqual(obj["code"], "EffectDenied", r.stderr)
                self.assertIn("denied:", obj["message"])

    def test_serve_error_uses_the_same_code_and_span(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "--serve"], input="(car 5)\n", capture_output=True, text=True, env=env, timeout=120
        )
        replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
        self.assertTrue(replies, p.stderr)
        obj = replies[-1]
        self.assertEqual(obj["status"], "error")
        self.assertEqual(obj["code"], "TypeError")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})
        self.assertIn("car: not a pair", obj["message"])

    def test_serve_async_error_uses_the_same_code_and_span(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        p = subprocess.run(
            [str(AURA_BIN), "--serve-async"],
            input='(error "x")\n',
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        replies = [json.loads(ln) for ln in p.stdout.splitlines() if ln.strip()]
        self.assertTrue(replies, p.stderr)
        obj = replies[-1]
        self.assertEqual(obj["status"], "error")
        self.assertEqual(obj["code"], "UserError")
        self.assertEqual(obj["message"], "x")
        self.assertEqual(obj["span"]["start"], {"line": 1, "column": 2})

    def test_division_by_zero_has_its_own_code(self) -> None:
        # #5241 batch 1: division by zero is typed at creation, not TypeError.
        r = run(["--error-format=json", "-e", "(/ 1 0)"])
        self.assertNotEqual(r.returncode, 0)
        obj = last_json(r.stderr)
        self.assertEqual(obj["code"], "DivisionByZero")
        self.assertIn("division by zero", obj["message"])
        for src in ("(modulo 1 0)", "(quotient 1 0)", "(remainder 1 0)"):
            r = run(["--error-format=json", "-e", src])
            self.assertEqual(last_json(r.stderr)["code"], "DivisionByZero", src)

    def test_pair_and_arithmetic_kinds_are_typed_at_creation(self) -> None:
        # #5241 batch 3: pair-shape and arithmetic operand mistakes carry
        # TypeError from creation. A program's own error whose text names
        # one of those categories stays UserError — the label is the kind
        # the producer set, never the message text.
        for src in ("(car 5)", "(cdr 5)", "(assv 1 5)", "(modulo 1 (list 1))", '(abs "x")'):
            with self.subTest(src=src):
                r = run(["--error-format=json", "-e", src])
                self.assertNotEqual(r.returncode, 0, src)
                self.assertEqual(last_json(r.stderr)["code"], "TypeError", src)
        # A program's own error keeps UserError even when its text is the
        # old classifier's phrase: the label is the creation kind, never a
        # prefix match on message text.
        for src in (
            '(error "car: not a pair")',
            '(error "division by zero")',
            '(error "effect-denied: io-read")',
            '(error "naked mutate: set-car!")',
        ):
            with self.subTest(src=src):
                r = run(["--error-format=json", "-e", src])
                self.assertEqual(last_json(r.stderr)["code"], "UserError", src)

    def test_car_not_a_pair_same_label_direct_and_via_define(self) -> None:
        # #5241 acceptance: (car 5) reads the same whether the error is
        # the form's value or bound by a define and displayed later.
        direct = run(["--error-format=json", "-e", "(car 5)"])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "w.aura"
            p.write_text("(define r (car 5))\n(display r)\n")
            wrapped = run(["--error-format=json", str(p)], cwd=d)
            wrapped_text = run([str(p)], cwd=d)
        a, b = last_json(direct.stderr), last_json(wrapped.stderr)
        self.assertEqual(a["code"], "TypeError")
        self.assertEqual(a["code"], b["code"])
        self.assertEqual(a["message"], b["message"])
        # The human prefix comes from the same kind on both paths.
        self.assertIn("type error: car: not a pair", run(["-e", "(car 5)"]).stderr)
        self.assertIn("type error: car: not a pair", wrapped_text.stderr)

    def test_module_failure_is_module_error_on_both_faces(self) -> None:
        # #5241 batch 4: a module that could not load is ModuleError from
        # creation. An import that returns the load failure as a value used
        # to reach the face as TypeError; the direct face now names the
        # category in the JSON code, --load in the human prefix.
        imported = run(["--error-format=json", "-e", '(import "./no-such-module-5241")'])
        self.assertNotEqual(imported.returncode, 0)
        self.assertEqual(last_json(imported.stderr)["code"], "ModuleError", imported.stderr)
        src = '(require "no-such-module-5241")\n'
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.aura"
            p.write_text(src)
            direct = run(["--error-format=json", str(p)], cwd=d)
            self.assertNotEqual(direct.returncode, 0)
            self.assertEqual(last_json(direct.stderr)["code"], "ModuleError", direct.stderr)
            loaded = run(["--load", str(p)], cwd=d)
            self.assertNotEqual(loaded.returncode, 0)
            self.assertIn("module error: require: cannot resolve", loaded.stderr)

    def test_exit_code_table(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            self.assertEqual(run([str(p)], cwd=d).returncode, 3)  # parse
        self.assertEqual(run(["-e", "(display zz)"]).returncode, 4)  # static/unbound
        self.assertEqual(run(["-e", "(car 5)"]).returncode, 1)  # runtime
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        empty = subprocess.run([str(AURA_BIN)], input="", capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(empty.returncode, 2)  # usage

    def test_exit_code_usage_errors(self) -> None:
        # Issue #5240: the one-shot table puts usage errors at 2. These two
        # paths used to fall through the reader with the old runtime value 1.
        self.assertEqual(run(["--definitely-not-an-option"]).returncode, 2)
        self.assertEqual(run(["/nonexistent-5240.aura"]).returncode, 2)

    def test_default_text_output_is_unchanged(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            p.write_text('(display "abc)\n')
            r = run([str(p)], cwd=d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("error:", r.stderr)
            self.assertIn("^", r.stderr)
            self.assertNotIn('"code":', r.stderr)

    def test_schema_fields_are_typed_and_one_object_per_line(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "u.aura"
            src = '(display "abc)\n'
            p.write_text(src)
            stderr = run(["--error-format=json", str(p)], cwd=d).stderr
            # NDJSON: every JSON-looking line is a complete object.
            objs = [json.loads(ln) for ln in stderr.splitlines() if ln.startswith("{")]
            self.assertEqual(len(objs), 1, stderr)
            obj = objs[0]
            self.assertEqual(set(obj) - {"hint", "notes", "snippet"}, {"code", "severity", "file", "span", "message"})
            self.assertIsInstance(obj["code"], str)
            self.assertEqual(obj["severity"], "error")
            self.assertEqual(obj["file"], str(p))
            self.assertIsInstance(obj["message"], str)
            self.assertEqual(set(obj["span"]), {"start", "end"})
            for edge in ("start", "end"):
                self.assertEqual(set(obj["span"][edge]), {"line", "column"})
                self.assertIsInstance(obj["span"][edge]["line"], int)
                self.assertIsInstance(obj["span"][edge]["column"], int)
            self.assertEqual(obj["span"]["start"], {"line": 1, "column": 10})
            self.assertEqual(obj["snippet"], src.rstrip("\n"))

    def test_span_columns_are_utf8_bytes(self) -> None:
        # #5240: the schema pins 1-based UTF-8 byte columns (#5258 converts
        # for LSP). é is 2 bytes, the emoji is 4; the column must follow the
        # bytes, not code points or UTF-16 units.
        for prefix in ("é", "🎉"):
            src = f'(display "{prefix}" (undefinedfn 5))\n'
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "u.aura"
                p.write_text(src)
                obj = last_json(run(["--error-format=json", str(p)], cwd=d).stderr)
                self.assertEqual(obj["code"], "UnboundVariable", src)
                self.assertEqual(obj["span"]["start"]["column"], src.encode("utf-8").index(b"undefinedfn") + 1, src)

    def test_serve_kind_is_the_ident_and_node_id_is_gone(self) -> None:
        env = dict(os.environ)
        env["AURA_SANDBOX"] = "off"
        proc = subprocess.run(
            [str(AURA_BIN), "--serve"], input="(car 5)\n", capture_output=True, text=True, env=env, timeout=120
        )
        obj = [json.loads(ln) for ln in proc.stdout.splitlines() if ln.strip()][-1]
        self.assertEqual(obj["status"], "error")
        self.assertEqual(obj["kind"], "TypeError")
        self.assertNotIn("node_id", obj)
        cli = last_json(run(["--error-format=json", "-e", "(car 5)"]).stderr)
        self.assertEqual(obj["code"], cli["code"])
        self.assertEqual(obj["message"], cli["message"])
        self.assertEqual(obj["span"], cli["span"])

    def test_lsp_uses_the_cli_code_and_span(self) -> None:
        src = "(display (memq 1 (list 1 2)))\n"
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.aura"
            p.write_text(src)
            uri = p.as_uri()
            msgs = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": Path(d).as_uri()}},
                {"jsonrpc": "2.0", "method": "initialized", "params": {}},
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": uri, "languageId": "aura", "version": 1, "text": src}},
                },
                {"jsonrpc": "2.0", "id": 3, "method": "shutdown"},
                {"jsonrpc": "2.0", "method": "exit"},
            ]
            env = dict(os.environ)
            env.pop("AURA_SANDBOX", None)
            env["AURA_RUNTIME_DIR"] = str(ROOT)
            proc = subprocess.run(
                [str(AURA_BIN), "lsp"],
                input=b"".join(frame(m) for m in msgs),
                capture_output=True,
                env=env,
                timeout=120,
            )
            diags = [
                f["params"] for f in read_frames(proc.stdout) if f.get("method") == "textDocument/publishDiagnostics"
            ]
            mine = next(x for x in diags if x["uri"] == uri)["diagnostics"]
            self.assertEqual(len(mine), 1, mine)
            cli = last_json(run(["--error-format=json", str(p)], cwd=d).stderr)
            self.assertEqual(cli["code"], "UnboundVariable")
            self.assertEqual(mine[0]["code"], cli["code"])
            # LSP characters are 0-based; the schema is 1-based bytes.
            self.assertEqual(mine[0]["range"]["start"]["character"] + 1, cli["span"]["start"]["column"])
            self.assertIn(cli["message"], mine[0]["message"])


if __name__ == "__main__":
    unittest.main()
