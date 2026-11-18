"""Hover, signatureHelp and completion cases for aura lsp (#5261).

The text comes from two places only: the A2 reader index (scopes, defines,
require links) and the compiler's primitive registry, which is also what the
REPL's :doc prints. The hover-vs-:doc case here is the consistency check: it
runs both faces of the same binary and compares the description.

Everything drives the real binary over Content-Length framed stdio.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from lsp_protocol import Client
from lsp_symbols import at, request

ROOT = Path(__file__).resolve().parents[3]
AURA = ROOT / "build" / "aura"


def hover(c: Client, uri: str, line: int, character: int) -> object:
    return request(c, "textDocument/hover", at(uri, line, character), rid=31)


def signature_help(c: Client, uri: str, line: int, character: int) -> object:
    return request(c, "textDocument/signatureHelp", at(uri, line, character), rid=32)


def completion(c: Client, uri: str, line: int, character: int) -> object:
    return request(c, "textDocument/completion", at(uri, line, character), rid=33)


def contents(result: object) -> str:
    assert isinstance(result, dict), result
    value = result["contents"]
    return value["value"] if isinstance(value, dict) else str(value)


def labels(items: object) -> dict[str, dict]:
    assert isinstance(items, list), items
    return {i["label"]: i for i in items}


def repl_doc(name: str) -> str:
    """:doc NAME as the REPL prints it (same binary, same registry)."""
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        e.pop(k, None)
    e["TERM"] = "dumb"
    r = subprocess.run(
        [str(AURA), "repl", "--quiet"],
        input=f":doc {name}\n(quit)\n",
        capture_output=True,
        text=True,
        env=e,
        timeout=180,
    )
    out = r.stdout
    # Drop the banner/echo lines: :doc prints its answer as one line.
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith(":doc") or line.startswith("aura") or line.startswith(">"):
            continue
        if "no description" in line or "no such primitive" in line:
            continue
        return line
    return ""


class LspSemantic5261(unittest.TestCase):
    def test_hover_matches_repl_doc_for_primitives(self) -> None:
        text = "(display 1)\n(map car (list 1))\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "prims.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                for line, name in ((0, "display"), (1, "map")):
                    doc = repl_doc(name)
                    got = contents(hover(c, uri, line, 1))
                    self.assertIn(name, got)
                    if doc:
                        # One description, two faces: hover carries :doc's text.
                        self.assertIn(doc, got, f"{name}: {got!r} vs :doc {doc!r}")
            finally:
                c.close()

    def test_hover_on_a_user_define_shows_params_and_the_comment_above(self) -> None:
        text = "; Doubles X.\n; Kept next to the definition.\n(define (double x) (* x 2))\n(display (double 3))\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "define.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                got = contents(hover(c, uri, 2, 10))
                self.assertIn("(double x)", got)
                self.assertIn("Doubles X.", got)
                self.assertIn("Kept next to the definition.", got)
                # Nothing to say inside a comment.
                self.assertIsNone(hover(c, uri, 0, 4))
            finally:
                c.close()

    def test_hover_on_a_require_string_shows_the_resolved_path(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "aura.toml").write_text("")
            (root / "lib.aura").write_text("(define (helper x) x)\n")
            text = '(require "lib")\n(display (helper 1))\n'
            path = root / "main.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                c.did_open(uri, text)
                got = contents(hover(c, uri, 0, 12))
                self.assertIn(str(root / "lib.aura"), got)
            finally:
                c.close()

    def test_signature_help_marks_the_current_argument(self) -> None:
        text = "(define (f a b) (+ a b))\n(f 1 2)\n(display\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "sig.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                # Inside the second argument of the user define.
                help_user = signature_help(c, uri, 1, 5)
                self.assertEqual(help_user["signatures"][0]["label"], "(f a b)")
                self.assertEqual([p["label"] for p in help_user["signatures"][0]["parameters"]], ["a", "b"])
                self.assertEqual(help_user["activeParameter"], 1)
                # A primitive call: the registry's signature is the label.
                help_prim = signature_help(c, uri, 2, 5)
                self.assertIn("display", help_prim["signatures"][0]["label"])
            finally:
                c.close()

    def test_completion_offers_primitives_defines_and_visible_bindings(self) -> None:
        text = (
            "; A helper for the test.\n"
            "(define (helper x) x)\n"
            "(define (caller y)\n"
            "  (let ((z 1)) (+ y z)))\n"
            "(display dis\n"
        )
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "complete.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                # Prefix "dis" at the end of the last line.
                items = labels(completion(c, uri, 4, 12))
                self.assertIn("display", items)
                self.assertEqual(items["display"]["kind"], 3)
                # Inside the caller body: y, the parameter, and z, the let name.
                params = labels(completion(c, uri, 3, 20))
                # A primitive the registry documents carries that description.
                self.assertIn("map", params)
                self.assertTrue(params["map"].get("documentation"))
                self.assertIn("y", params)
                self.assertIn("z", params)
                self.assertEqual(params["z"]["kind"], 6)
                # Top-level defines are candidates too, with their comment.
                top = labels(completion(c, uri, 3, 20))
                self.assertIn("helper", top)
                self.assertIn("A helper for the test.", json.dumps(top["helper"]))
            finally:
                c.close()

    def test_completion_lists_require_paths_from_root_std_and_aura_path(self) -> None:
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as ap:
            root = Path(d)
            (root / "aura.toml").write_text("")
            (root / "mylib.aura").write_text("(define x 1)\n")
            extra = Path(ap)
            (extra / "extramod.aura").write_text("(define y 2)\n")
            text = '(require "'
            path = root / "main.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client(extra_env={"AURA_PATH": str(extra)})
            try:
                c.initialize(root=root.as_uri())
                c.did_open(uri, text)
                items = labels(completion(c, uri, 0, len(text)))
                self.assertIn("std/list", items, sorted(items)[:20])
                self.assertIn("mylib", items)
                self.assertIn("extramod", items)
                self.assertEqual(items["mylib"]["kind"], 17)
                # The documentation is the path A1 resolution found on disk.
                self.assertEqual(items["mylib"]["detail"], str(root / "mylib.aura"))
                self.assertIn("mylib.aura", json.dumps(items["mylib"]))
                self.assertEqual(items["extramod"]["detail"], str(extra / "extramod.aura"))
            finally:
                c.close()

    def test_capabilities_advertise_hover_signature_and_completion(self) -> None:
        c = Client()
        try:
            caps = c.initialize()["capabilities"]
            self.assertTrue(caps["hoverProvider"])
            self.assertIn("(", caps["signatureHelpProvider"]["triggerCharacters"])
            self.assertIn('"', caps["completionProvider"]["triggerCharacters"])
        finally:
            c.close()
