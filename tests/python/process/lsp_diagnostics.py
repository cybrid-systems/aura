"""Diagnostics upgrade for aura lsp (#5262).

The server consumes the structured diagnostic (#5240) instead of re-parsing
typecheck output: one code (the stable kind ident the CLI and serve JSON faces
print), the kind's severity, the producer's message with the hint kept apart,
relatedInformation for the require chain, and a did-you-mean quickfix. These
cases drive the real binary over framed stdio and compare it with the sibling
faces of the same diagnostic.
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from lsp_protocol import AURA, Client, env, last_publication

MEMQ = "(display (memq 1 (list 1 2)))\n"
ARITY = "(define (f x) x)\n(display (f))\n"
PARSE = "(display (+ 1 2)\n"
TYPE_MISMATCH = '(define (g y) (+ y "s"))\n'


def run_aura(args: list[str], cwd: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(AURA), *args], cwd=cwd, capture_output=True, text=True, env=env(), timeout=120)


def last_json(text: str) -> dict:
    lines = [ln for ln in text.splitlines() if ln.startswith("{")]
    assert lines, text
    return json.loads(lines[-1])


def open_diagnostics(text: str, init_opts: dict | None = None) -> tuple[list[dict], str, str]:
    """Open one buffer in a temp project; return (diagnostics, dir, uri)."""
    d = tempfile.mkdtemp(prefix="lspdiag5262-")
    path = Path(d) / "a.aura"
    path.write_text(text)
    uri = path.as_uri()
    c = Client()
    try:
        c.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "capabilities": {"general": {"positionEncodings": ["utf-16"]}},
                    "rootUri": Path(d).as_uri(),
                    "initializationOptions": init_opts or {},
                },
            }
        )
        c.pump(2.0)
        c.did_open(uri, text)
        frames = c.pump(2.0)
        pub = last_publication(frames, uri)
        assert pub is not None, frames
        return pub["diagnostics"], d, uri
    finally:
        c.close()


def code_action(uri: str, line: int, character: int = 0) -> list[dict]:
    c = Client()
    try:
        c.send(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"capabilities": {}, "initializationOptions": {}},
            }
        )
        c.pump(2.0)
        c.clear()
        # didOpen re-analyzes; the action uses the same last analysis.
        c.did_open(uri, Path(uri[7:]).read_text())
        c.pump(1.0)
        c.clear()
        c.send(
            {
                "jsonrpc": "2.0",
                "id": 77,
                "method": "textDocument/codeAction",
                "params": {
                    "textDocument": {"uri": uri},
                    "range": {
                        "start": {"line": line, "character": character},
                        "end": {"line": line, "character": character},
                    },
                    "context": {"diagnostics": []},
                },
            }
        )
        for f in c.pump(2.0):
            if f.get("id") == 77:
                return f.get("result") or []
        raise AssertionError("no codeAction reply")
    finally:
        c.close()


class LspDiagnostics5262(unittest.TestCase):
    def test_codes_severity_and_message_match_the_json_faces(self) -> None:
        # File JSON and --serve build the same diagnostic from the same
        # structured record; the LSP wire position is 0-based.
        diags, d, _uri = open_diagnostics(MEMQ)
        self.assertEqual(len(diags), 1, diags)
        dx = diags[0]
        self.assertEqual(dx["code"], "UnboundVariable")
        self.assertEqual(dx["severity"], 1)
        self.assertEqual(dx["message"], "memq")
        self.assertNotIn("did you mean", dx["message"])
        self.assertEqual(dx["data"]["suggestion"], "member")

        obj = last_json(run_aura(["--error-format=json", "a.aura"], d).stderr)
        self.assertEqual(obj["code"], dx["code"])
        self.assertEqual(obj["message"], dx["message"])
        self.assertEqual(obj["span"]["start"]["line"] - 1, dx["range"]["start"]["line"])
        self.assertEqual(obj["span"]["start"]["column"] - 1, dx["range"]["start"]["character"])

        served = json.loads(
            subprocess.run(
                [str(AURA), "--serve"],
                input=MEMQ,
                capture_output=True,
                text=True,
                env=env(),
                timeout=120,
            ).stdout.splitlines()[-1]
        )
        self.assertEqual(served["status"], "error")
        self.assertEqual(served["code"], dx["code"])
        self.assertEqual(served["message"], dx["message"])
        self.assertEqual(served["span"]["start"], obj["span"]["start"])

    def test_unbound_arity_and_parse_errors_are_errors(self) -> None:
        for text, code, message in (
            (MEMQ, "UnboundVariable", "memq"),
            (ARITY, "ArityMismatch", None),
            (PARSE, "ParseError", None),
        ):
            with self.subTest(code=code):
                diags, _d, _uri = open_diagnostics(text)
                self.assertEqual(len(diags), 1, diags)
                self.assertEqual(diags[0]["code"], code)
                self.assertEqual(diags[0]["severity"], 1, diags[0])
                if message is not None:
                    self.assertEqual(diags[0]["message"], message)
        # The parse-error message is the producer's, with no kind prefix.
        diags, d, _uri = open_diagnostics(PARSE)
        obj = last_json(run_aura(["--error-format=json", "a.aura"], d).stderr)
        self.assertEqual(obj["code"], "ParseError")
        self.assertEqual(obj["message"], diags[0]["message"])
        self.assertTrue(diags[0]["message"].startswith("unclosed list"))

    def test_type_diagnostic_is_a_warning_that_matches_typecheck(self) -> None:
        diags, d, _uri = open_diagnostics(TYPE_MISMATCH)
        self.assertEqual(len(diags), 1, diags)
        self.assertEqual(diags[0]["code"], "TypeError")
        self.assertEqual(diags[0]["severity"], 2, diags[0])
        message = diags[0]["message"]
        self.assertIn("expected Int", message)
        self.assertNotIn("type error:", message)
        # --typecheck (the face #5250 aligned) prints the same message.
        tc = run_aura(["--typecheck", "a.aura"], d)
        self.assertIn("type error: " + message, tc.stdout)
        self.assertNotEqual(tc.returncode, 0)

    def test_type_mode_off_and_error_from_both_configuration_channels(self) -> None:
        diags, _d, _uri = open_diagnostics(TYPE_MISMATCH, {"typeDiagnostics": "off"})
        self.assertEqual(diags, [])
        diags, _d, _uri = open_diagnostics(TYPE_MISMATCH, {"typeDiagnostics": "error"})
        self.assertEqual(len(diags), 1, diags)
        self.assertEqual(diags[0]["severity"], 1, diags[0])

        # workspace/didChangeConfiguration re-analyzes open documents.
        d = tempfile.mkdtemp(prefix="lspdiagcfg5262-")
        path = Path(d) / "a.aura"
        path.write_text(TYPE_MISMATCH)
        uri = path.as_uri()
        c = Client()
        try:
            c.send(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"capabilities": {}, "rootUri": Path(d).as_uri()},
                }
            )
            c.pump(2.0)
            c.clear()
            c.did_open(uri, TYPE_MISMATCH)
            pub = last_publication(c.pump(2.0), uri)
            self.assertEqual(pub["diagnostics"][0]["severity"], 2, pub)
            c.clear()
            c.send(
                {
                    "jsonrpc": "2.0",
                    "method": "workspace/didChangeConfiguration",
                    "params": {"settings": {"typeDiagnostics": "off"}},
                }
            )
            pub = last_publication(c.pump(2.0), uri)
            self.assertEqual(pub["diagnostics"], [], pub)
            c.clear()
            c.send(
                {
                    "jsonrpc": "2.0",
                    "method": "workspace/didChangeConfiguration",
                    "params": {"settings": {"aura": {"typeDiagnostics": "error"}}},
                }
            )
            pub = last_publication(c.pump(2.0), uri)
            self.assertEqual(pub["diagnostics"][0]["severity"], 1, pub)
        finally:
            c.close()

    def test_require_chain_is_related_information(self) -> None:
        d = Path(tempfile.mkdtemp(prefix="lspdiagreq5262-"))
        lib = d / "lib.aura"
        lib.write_text(MEMQ)
        main = d / "main.aura"
        main.write_text('(require "lib" all:)\n(display 1)\n')
        uri = main.as_uri()
        c = Client()
        try:
            c.send(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"capabilities": {}, "rootUri": d.as_uri()},
                }
            )
            c.pump(2.0)
            c.clear()
            c.did_open(uri, main.read_text())
            frames = c.pump(2.0)
            pub = last_publication(frames, lib.as_uri())
            self.assertIsNotNone(pub, frames)
            dx = pub["diagnostics"][0]
            self.assertEqual(dx["code"], "UnboundVariable")
            related = dx["relatedInformation"]
            self.assertTrue(related, dx)
            self.assertEqual(related[0]["location"]["uri"], uri)
            self.assertEqual(related[0]["location"]["range"]["start"]["line"], 0)
            self.assertIn("required here", related[0]["message"])
        finally:
            c.close()

    def test_did_you_mean_quickfix_replaces_the_name(self) -> None:
        diags, _d, uri = open_diagnostics(MEMQ)
        actions = code_action(uri, 0, 10)
        self.assertEqual(len(actions), 1, actions)
        action = actions[0]
        self.assertEqual(action["kind"], "quickfix")
        self.assertNotIn("command", action, "the quickfix must not call a primitive")
        changes = action["edit"]["changes"]
        self.assertEqual(list(changes), [uri])
        edit = changes[uri][0]
        self.assertEqual(edit["newText"], "member")
        self.assertEqual(edit["range"], diags[0]["range"])
        before = Path(uri[7:]).read_text()
        start = edit["range"]["start"]
        end = edit["range"]["end"]
        lines = before.splitlines(keepends=True)
        line = lines[start["line"]]
        lines[start["line"]] = line[: start["character"]] + edit["newText"] + line[end["character"] :]
        after = "".join(lines)
        self.assertEqual(after, "(display (member 1 (list 1 2)))\n")
        # Text only: the server did not rewrite the file.
        self.assertEqual(Path(uri[7:]).read_text(), before)
        self.assertEqual(diags[0]["code"], "UnboundVariable")
