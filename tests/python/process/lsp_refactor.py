"""references / prepareRename / rename cases for aura lsp (#5260).

One declaration in geom.aura, a use in the declaring file, and two requiring
files (report.aura and main.aura). The reader index decides what a name is, so
a mention in a comment or a string literal never appears, and the let binding
in report.aura that shadows the name stays a different binding.

The rename answer is a WorkspaceEdit the client applies: the cases assert the
files on disk are byte-identical after the request and that the server never
asks for an applyEdit or a mutate:* primitive (#5252 owns live-workspace
edits; this surface only produces text).
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from lsp_protocol import AURA, Client, env
from lsp_symbols import at

# display prints without a separator, so each program line ends with an
# explicit newline: the run output then pins the order the files execute in.
GEOM = (
    "; area is a word in this comment, not a reference\n"
    "(define (area r)\n"
    "  (* r r))\n"
    '(define note "area")\n'
    '(display (area 3))(display "|")\n'
)

REPORT = (
    '(require "geom")\n'
    "(define (scaled)\n"
    "  (let ((area 10))\n"
    "    (+ area area)))\n"
    '(display (scaled))(display "|")\n'
    '(display (area 4))(display "|")\n'
)

MAIN = (
    '(require "geom")\n'
    '(require "report")\n'
    '(display (area 5))(display "|")\n'
    # A lambda parameter of the same name shadows the library define: neither
    # its binding nor its body is a reference to geom.aura's area.
    '(display ((lambda (area) area) 7))(display "|")\n'
)

# The three files, run in require order: geom prints 9, report prints 20 and
# 16, main prints 25.
PROGRAM_OUTPUT = "9|20|16|25|7|"


def call(c: Client, method: str, params: dict, rid: int = 41) -> dict:
    """One request, whole reply frame (the error cases need the message)."""
    start = len(c.frames)
    c.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    end = time.monotonic() + 20.0
    while time.monotonic() < end:
        for f in c.frames[start:]:
            if f.get("id") == rid:
                return f
        c.pump(0.05)
    raise AssertionError(f"no reply to {method}")


def reference_sites(result: object) -> list[tuple[str, int, int]]:
    assert isinstance(result, list), result
    return [
        (loc["uri"].rsplit("/", 1)[-1], loc["range"]["start"]["line"], loc["range"]["start"]["character"])
        for loc in result
    ]


def apply_edits(text: str, edits: list[dict]) -> str:
    starts = [0]
    for line in text.splitlines(keepends=True):
        starts.append(starts[-1] + len(line))

    def off(pos: dict) -> int:
        return starts[pos["line"]] + pos["character"]

    for edit in sorted(edits, key=lambda e: off(e["range"]["start"]), reverse=True):
        text = text[: off(edit["range"]["start"])] + edit["newText"] + text[off(edit["range"]["end"]) :]
    return text


def project(d: Path) -> tuple[Path, Path, Path]:
    (d / "aura.toml").write_text("")
    geom = d / "geom.aura"
    report = d / "report.aura"
    main = d / "main.aura"
    geom.write_text(GEOM)
    report.write_text(REPORT)
    main.write_text(MAIN)
    return geom, report, main


class LspRefactor5260(unittest.TestCase):
    def test_references_list_the_declaration_and_cross_require_uses(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            geom, _report, main = project(root)
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                # No didOpen: every file is on disk, so the server reads the
                # same text a client would send (and the case stays cheap).
                with_decl = call(
                    c,
                    "textDocument/references",
                    {**at(geom.as_uri(), 1, 10), "context": {"includeDeclaration": True}},
                )["result"]
                self.assertEqual(
                    reference_sites(with_decl),
                    [
                        ("geom.aura", 1, 9),
                        ("geom.aura", 4, 10),
                        ("main.aura", 2, 10),
                        ("report.aura", 5, 10),
                    ],
                    with_decl,
                )
                without = call(
                    c,
                    "textDocument/references",
                    {**at(geom.as_uri(), 1, 10), "context": {"includeDeclaration": False}},
                )["result"]
                self.assertEqual(len(without), 3, without)
                self.assertNotIn(("geom.aura", 1, 9), reference_sites(without))

                # A use works as an entry point too, and lands on the same set.
                from_use = call(
                    c,
                    "textDocument/references",
                    {**at(main.as_uri(), 2, 10), "context": {"includeDeclaration": True}},
                )["result"]
                self.assertEqual(len(from_use), 4, from_use)
            finally:
                c.close()

    def test_rename_produces_a_three_file_workspace_edit_and_the_program_runs(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            geom, report, main = project(root)
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                c.did_open(main.as_uri(), MAIN)
                before = {p: p.read_bytes() for p in (geom, report, main)}
                frame = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 1, 10), "newName": "zone"},
                    rid=42,
                )
                self.assertNotIn("error", frame, frame)
                changes = frame["result"]["changes"]
                self.assertEqual(
                    sorted(changes),
                    sorted(p.as_uri() for p in (geom, report, main)),
                    changes,
                )
                self.assertEqual([len(v) for v in changes.values()], [2, 1, 1], changes)
                for edit in [e for v in changes.values() for e in v]:
                    self.assertEqual(edit["newText"], "zone")
                # Nothing was written and nothing was applied by the server:
                # the answer is text, and the workspace is not #5252's surface.
                self.assertEqual(before, {p: p.read_bytes() for p in (geom, report, main)})
                dumped = json.dumps(c.frames)
                self.assertNotIn("applyEdit", dumped)
                self.assertNotIn("mutate:", dumped)

                # The client applies the edit; the program still runs as before.
                for path in (geom, report, main):
                    uri = path.as_uri()
                    path.write_text(apply_edits(path.read_text(), changes[uri]))
                self.assertIn("(define (zone r)", geom.read_text())
                self.assertIn("(display (zone 5))", main.read_text())
                # The shadowing let in report.aura was left alone.
                self.assertIn("(let ((area 10))", report.read_text())
                self.assertIn("(lambda (area) area)", main.read_text())
                self.assertIn("(display (zone 4))", report.read_text())
                run = subprocess.run(
                    [str(AURA), "main.aura"],
                    cwd=root,
                    capture_output=True,
                    text=True,
                    env=env(),
                    timeout=180,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stdout, PROGRAM_OUTPUT, run.stdout)
            finally:
                c.close()

    def test_comment_string_and_shadowed_local_are_not_references(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            geom, report, main = project(root)
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                sites = reference_sites(
                    call(
                        c,
                        "textDocument/references",
                        {**at(geom.as_uri(), 1, 10), "context": {"includeDeclaration": True}},
                    )["result"]
                )
                self.assertNotIn(("geom.aura", 0, 2), sites, "the comment")
                self.assertNotIn(("geom.aura", 3, 13), sites, "the string literal")
                self.assertNotIn(("report.aura", 2, 11), sites, "the let binding name")
                self.assertNotIn(("report.aura", 3, 7), sites, "the let-bound use")
                self.assertNotIn(("report.aura", 3, 13), sites, "the let-bound use")
                self.assertEqual(
                    [s for s in sites if s[0] == "main.aura" and s[1] == 3],
                    [],
                    "the lambda parameter and its body are a different binding",
                )
                # A position inside the comment is not a name at all.
                self.assertEqual(
                    call(
                        c,
                        "textDocument/prepareRename",
                        at(geom.as_uri(), 0, 4),
                        rid=44,
                    )
                    .get("error", {})
                    .get("code"),
                    -32602,
                )
            finally:
                c.close()

    def test_primitive_and_std_names_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            geom, _report, main = project(root)
            prelude = root / "prelude.aura"
            prelude.write_text("(display (foldr + 0 (list 1 2)))\n")
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                prim = call(c, "textDocument/prepareRename", at(main.as_uri(), 2, 2), rid=45)
                self.assertEqual(prim.get("error", {}).get("code"), -32602, prim)
                self.assertIn("primitive", prim["error"]["message"])
                self.assertIn("display", prim["error"]["message"])
                renamed = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 4, 10), "newName": "display"},
                    rid=46,
                )
                self.assertEqual(renamed.get("error", {}).get("code"), -32602, renamed)
                self.assertIn("primitive", renamed["error"]["message"])

                std = call(c, "textDocument/prepareRename", at(prelude.as_uri(), 0, 12), rid=47)
                self.assertEqual(std.get("error", {}).get("code"), -32602, std)
                self.assertIn("std", std["error"]["message"])
                self.assertIn("foldr", std["error"]["message"])
                self.assertEqual(
                    call(
                        c,
                        "textDocument/references",
                        {**at(prelude.as_uri(), 0, 12), "context": {"includeDeclaration": True}},
                        rid=48,
                    )["result"],
                    [],
                )
            finally:
                c.close()

    def test_illegal_and_conflicting_new_names_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            geom, report, main = project(root)
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                bad = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 1, 10), "newName": "2bad"},
                    rid=49,
                )
                self.assertEqual(bad.get("error", {}).get("code"), -32602, bad)
                self.assertIn("not a legal identifier", bad["error"]["message"])

                paren = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 1, 10), "newName": "a b"},
                    rid=50,
                )
                self.assertEqual(paren.get("error", {}).get("code"), -32602, paren)
                self.assertIn("not a legal identifier", paren["error"]["message"])

                # scaled is report.aura's own define, and report.aura is one of
                # the files this rename edits: taking the name would capture.
                clash = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 1, 10), "newName": "scaled"},
                    rid=51,
                )
                self.assertEqual(clash.get("error", {}).get("code"), -32602, clash)
                self.assertIn("already bound", clash["error"]["message"])
                self.assertIn("report.aura", clash["error"]["message"])

                # The same-name rename is a no-op, not an error.
                same = call(
                    c,
                    "textDocument/rename",
                    {**at(geom.as_uri(), 1, 10), "newName": "area"},
                    rid=52,
                )
                self.assertNotIn("error", same, same)
                self.assertEqual(len(same["result"]["changes"]), 3)
            finally:
                c.close()

    def test_references_follow_aura_path_and_std_require(self) -> None:
        with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as lib:
            root = Path(d)
            (root / "aura.toml").write_text("")
            helper = Path(lib) / "helper.aura"
            helper.write_text("(define (twice x) (+ x x))\n")
            main = root / "main.aura"
            main.write_text('(require "helper")\n(display (twice 2))(display "|")\n')
            stdref = root / "stdref.aura"
            stdref.write_text('(require "std/list")\n(display (foldr + 0 (list 1 2)))(display "|")\n')
            c = Client(extra_env={"AURA_PATH": lib})
            try:
                c.initialize(root=root.as_uri())
                c.did_open(main.as_uri(), main.read_text())
                via_path = reference_sites(
                    call(
                        c,
                        "textDocument/references",
                        {**at(main.as_uri(), 1, 10), "context": {"includeDeclaration": True}},
                    )["result"]
                )
                # Two temp directories: the file order is the path order, so
                # compare the set, not one particular interleaving.
                self.assertEqual(sorted(via_path), [("helper.aura", 0, 9), ("main.aura", 1, 10)], via_path)

                # A (require "std/list") file is a dependent like any other: the
                # declaration lives in the standard library, and the project use
                # is a reference to it.
                std = call(
                    c,
                    "textDocument/references",
                    {**at(stdref.as_uri(), 1, 10), "context": {"includeDeclaration": True}},
                    rid=53,
                )["result"]
                self.assertTrue(any(loc["uri"].endswith("/lib/std/list.aura") for loc in std), std)
                self.assertTrue(any(loc["uri"].endswith("/stdref.aura") for loc in std), std)

                # Renaming the std definition is refused even from a use site.
                refused = call(c, "textDocument/prepareRename", at(stdref.as_uri(), 1, 10), rid=54)
                self.assertEqual(refused.get("error", {}).get("code"), -32602, refused)
                self.assertIn("standard library", refused["error"]["message"])
            finally:
                c.close()

    def test_capabilities_advertise_references_and_rename(self) -> None:
        c = Client()
        try:
            caps = c.initialize()
            self.assertIs(caps["capabilities"]["referencesProvider"], True)
            self.assertEqual(caps["capabilities"]["renameProvider"], {"prepareProvider": True})
        finally:
            c.close()
