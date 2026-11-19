"""Symbol-layer cases for aura lsp (#5259).

The index is the reader's: comments and string literals never produce
definitions, nested defines belong to their enclosing scope, and the outline
/ workspace symbol / folding / selection / link capabilities are queries over
the same tree. Everything here drives the real binary over Content-Length
framed stdio.
"""

from __future__ import annotations

import os
import select
import tempfile
import time
import unittest
from pathlib import Path

from lsp_protocol import Client, publications, starts


def _frames_after(c: Client, start: int) -> list[dict]:
    """Pump until a new frame arrives; no fixed sleep, so the theme stays
    well inside its process-tier timeout."""
    end = time.monotonic() + 20.0
    while time.monotonic() < end:
        if len(c.frames) > start:
            return c.frames[start:]
        try:
            ready, _, _ = select.select([c.p.stdout], [], [], 0.05)
        except (ValueError, OSError):
            break
        if not ready:
            continue
        chunk = os.read(c.p.stdout.fileno(), 1 << 16)
        if not chunk:
            break
        c.buf += chunk
        c._parse()
    return c.frames[start:]


def request(c: Client, method: str, params: dict, rid: int = 21) -> object:
    start = len(c.frames)
    c.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    end = time.monotonic() + 20.0
    while time.monotonic() < end:
        for f in c.frames[start:]:
            if f.get("id") == rid:
                return f.get("result")
        # The didOpen publication can arrive before the reply; keep pumping.
        _frames_after(c, len(c.frames))
    return "no reply"


def definition(c: Client, uri: str, line: int, character: int) -> object:
    return request(c, "textDocument/definition", at(uri, line, character))


def wait_publication(c: Client, uri: str, start: int = 0) -> object:
    end = time.monotonic() + 20.0
    seen = start
    while time.monotonic() < end:
        found = [p for p in publications(c.frames[seen:]) if p.get("uri") == uri]
        if found:
            return found[-1]
        seen = len(c.frames)
        _frames_after(c, seen)
    return None


def at(uri: str, line: int, character: int) -> dict:
    return {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}}


def names(symbols: object) -> list[str]:
    assert isinstance(symbols, list), symbols
    return [s["name"] for s in symbols]


class LspSymbols5259(unittest.TestCase):
    def test_comment_and_string_defines_are_not_symbols(self) -> None:
        text = '; (define ghost 1)\n#| (define ghost 2) |#\n(display "(define ghost 3)")\n(display ghost)\n'
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "ghost.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                before = len(c.frames)
                c.did_open(uri, text)
                pub = wait_publication(c, uri, before)
                self.assertIsNotNone(pub)
                self.assertEqual(starts(pub), [(3, 9)], "the comment must not bind ghost")

                pulled = request(c, "textDocument/diagnostic", {"textDocument": {"uri": uri}}, rid=22)
                self.assertEqual(
                    [(i["range"]["start"]["line"], i["range"]["start"]["character"]) for i in pulled["items"]],
                    [(3, 9)],
                )

                self.assertIsNone(definition(c, uri, 3, 10), "ghost has no definition")
                self.assertIsNone(definition(c, uri, 0, 11), "a position inside a comment")
                self.assertIsNone(definition(c, uri, 1, 11), "a position inside a block comment")
                self.assertEqual(names(request(c, "textDocument/documentSymbol", {"textDocument": {"uri": uri}})), [])
            finally:
                c.close()

    def test_definition_forms_and_the_two_that_are_not(self) -> None:
        text = (
            "(define-hygienic-macro (twice x) (+ x x))\n"
            "(define-type Tree (leaf val) (node left right))\n"
            "(define-struct point (x y))\n"
            "(display (twice 2))\n"
        )
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "forms.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                symbols = request(c, "textDocument/documentSymbol", {"textDocument": {"uri": uri}})
                self.assertEqual(names(symbols), ["twice", "Tree", "point"])
                kinds = {s["name"]: s["kind"] for s in symbols}
                self.assertEqual(kinds["twice"], 12, "a macro with parameters is a function")
                self.assertEqual(kinds["Tree"], 23, "define-type is a struct symbol")
                self.assertEqual(kinds["point"], 23, "define-struct is a struct symbol")

                target = definition(c, uri, 3, 10)
                self.assertIsInstance(target, dict, target)
                self.assertEqual((target["range"]["start"]["line"], target["range"]["start"]["character"]), (0, 24))
            finally:
                c.close()

        legacy = "(define-syntax legacy 1)\n(define-record-type rec (make-rec) rec?)\n(display rec)\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "legacy.aura"
            path.write_text(legacy)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, legacy)
                self.assertEqual(
                    names(request(c, "textDocument/documentSymbol", {"textDocument": {"uri": uri}})),
                    [],
                    "define-syntax / define-record-type are not runtime definitions",
                )
                self.assertIsNone(definition(c, uri, 2, 10))
            finally:
                c.close()

    def test_nested_define_is_a_child_of_its_enclosing_scope(self) -> None:
        text = "(define (outer x)\n  (define (inner y) (+ x y))\n  (inner x))\n(define second 2)\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "nested.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                symbols = request(c, "textDocument/documentSymbol", {"textDocument": {"uri": uri}})
                self.assertEqual(names(symbols), ["outer", "second"])
                self.assertEqual(names(symbols[0].get("children", [])), ["inner"])
                self.assertNotIn("inner", names(symbols))

                # The nested define is reachable, and it resolves to its own line.
                target = definition(c, uri, 2, 3)
                self.assertIsInstance(target, dict, target)
                self.assertEqual(target["range"]["start"]["line"], 1)
            finally:
                c.close()

    def test_let_and_lambda_bindings_definition_and_highlight(self) -> None:
        text = "(define (f a)\n  (let ((b 1))\n    (+ b a b)))\n(define g (lambda (c) (+ c 1)))\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "local.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)

                b = definition(c, uri, 2, 7)
                self.assertIsInstance(b, dict, b)
                self.assertEqual((b["range"]["start"]["line"], b["range"]["start"]["character"]), (1, 9))

                hits = request(c, "textDocument/documentHighlight", at(uri, 2, 7))
                got = sorted((h["range"]["start"]["line"], h["range"]["start"]["character"], h["kind"]) for h in hits)
                self.assertEqual(got, [(1, 9, 3), (2, 7, 2), (2, 11, 2)])

                c_param = definition(c, uri, 3, 25)
                self.assertIsInstance(c_param, dict, c_param)
                self.assertEqual((c_param["range"]["start"]["line"], c_param["range"]["start"]["character"]), (3, 19))
                param_hits = request(c, "textDocument/documentHighlight", at(uri, 3, 25))
                self.assertEqual(len(param_hits), 2, param_hits)
            finally:
                c.close()

    def test_highlight_collects_cross_file_uses_in_this_document(self) -> None:
        """#5360: a name declared by a required file is highlighted here.

        The reader cannot resolve a cross-file reference (binding -1), so the
        document's own occurrences came back empty even though the declaring
        file is one require away.  A let/lambda binding of the same spelling is
        resolved, so it is not one of those uses.
        """
        text_a = "(define (pa q) (car q))\n"
        text_b = (
            '(require "lib/a")\n'
            "(define (f x)\n"
            "  (let ((y (pa (pa x))))\n"
            "    (pa y)))\n"
            "(define (k r) (let ((z (add r (pa r) r))) z))\n"
            "(define (add a b c) (+ a b c))\n"
            "(define (shadow pa) (pa 1))\n"
        )
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "aura.toml").write_text("")
            (root / "lib").mkdir()
            (root / "lib" / "a.aura").write_text(text_a)
            path = root / "lib" / "b.aura"
            path.write_text(text_b)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize(root=root.as_uri())
                c.did_open(uri, text_b)
                hits = request(c, "textDocument/documentHighlight", at(uri, 3, 5))
                got = sorted((h["range"]["start"]["line"], h["range"]["start"]["character"], h["kind"]) for h in hits)
                self.assertEqual(got, [(2, 12, 2), (2, 16, 2), (3, 5, 2), (4, 31, 2)])
                # The shadowing parameter of (define (shadow pa) …) is a local
                # binding, not a use of the required define.
                shadow = request(c, "textDocument/documentHighlight", at(uri, 6, 17))
                shadow_hits = sorted(
                    (h["range"]["start"]["line"], h["range"]["start"]["character"], h["kind"]) for h in shadow
                )
                self.assertEqual(shadow_hits, [(6, 16, 3), (6, 21, 2)])
                # A primitive is still not a name this layer highlights, and
                # neither is a position inside the require string.
                self.assertEqual(request(c, "textDocument/documentHighlight", at(uri, 5, 21)), [])
                self.assertEqual(request(c, "textDocument/documentHighlight", at(uri, 0, 3)), [])
            finally:
                c.close()

    def test_workspace_symbol_sees_the_project_and_required_files(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            proj = Path(d) / "proj"
            proj.mkdir()
            (proj / "aura.toml").write_text('[project]\nname = "p"\n')
            main = proj / "main.aura"
            main.write_text('(require "lib")\n(define hello 1)\n(display (helper 2))\n')
            (proj / "lib.aura").write_text("(define (helper x) (+ x 1))\n")
            uri = main.as_uri()
            c = Client()
            try:
                c.initialize({}, proj.as_uri())
                c.did_open(uri, main.read_text())
                found = request(c, "workspace/symbol", {"query": "hel"})
                self.assertEqual(sorted(s["name"] for s in found), ["hello", "helper"])
                uris = {s["name"]: s["location"]["uri"] for s in found}
                self.assertTrue(uris["helper"].endswith("lib.aura"), uris)
                self.assertTrue(uris["hello"].endswith("main.aura"), uris)
                self.assertEqual(request(c, "workspace/symbol", {"query": "zzz"}), [])
            finally:
                c.close()

    def test_folding_and_selection_ranges(self) -> None:
        text = "#| block\ncomment |#\n(define (f x)\n  (let ((y x))\n    (+ x y)))\n(display (f 1))\n"
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "fold.aura"
            path.write_text(text)
            uri = path.as_uri()
            c = Client()
            try:
                c.initialize()
                c.did_open(uri, text)
                folds = request(c, "textDocument/foldingRange", {"textDocument": {"uri": uri}})
                self.assertIn({"startLine": 0, "endLine": 1, "kind": "comment"}, folds)
                self.assertIn({"startLine": 2, "endLine": 4, "kind": "region"}, folds)
                self.assertIn({"startLine": 3, "endLine": 4, "kind": "region"}, folds)

                sel = request(
                    c,
                    "textDocument/selectionRange",
                    {"textDocument": {"uri": uri}, "positions": [{"line": 4, "character": 6}]},
                )
                self.assertIsInstance(sel, list, sel)
                node = sel[0]
                depth = 0
                prev = None
                while node:
                    rng = node["range"]
                    if prev is not None:
                        # prev is the inner range, rng the outer one: the
                        # inner starts no earlier and ends no later.
                        self.assertGreaterEqual(
                            (prev["start"]["line"], prev["start"]["character"]),
                            (rng["start"]["line"], rng["start"]["character"]),
                        )
                        self.assertLessEqual(
                            (prev["end"]["line"], prev["end"]["character"]),
                            (rng["end"]["line"], rng["end"]["character"]),
                        )
                    prev = rng
                    depth += 1
                    node = node.get("parent")
                self.assertGreaterEqual(depth, 3, sel)
                self.assertEqual(prev["start"]["line"], 2, "the outermost range is the top-level form")
            finally:
                c.close()

    def test_document_link_resolves_require_paths(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            proj = Path(d) / "proj"
            proj.mkdir()
            (proj / "aura.toml").write_text('[project]\nname = "p"\n')
            (proj / "lib.aura").write_text("(define lib-name 1)\n")
            main = proj / "main.aura"
            text = '(require "lib")\n(display lib-name)\n; (require "missing")\n'
            main.write_text(text)
            uri = main.as_uri()
            c = Client()
            try:
                c.initialize({}, proj.as_uri())
                c.did_open(uri, text)
                links = request(c, "textDocument/documentLink", {"textDocument": {"uri": uri}})
                self.assertEqual(len(links), 1, "a commented require is not a link")
                link = links[0]
                self.assertEqual(link["range"]["start"], {"line": 0, "character": 10})
                self.assertEqual(link["range"]["end"], {"line": 0, "character": 13})
                self.assertTrue(link["target"].endswith("lib.aura"), link)
            finally:
                c.close()

    def _timed_symbol_request(self, text: str, method: str, runs: int = 3) -> float:
        """Median request time in a fresh server, after the first publication."""
        times = []
        for _ in range(runs):
            root = tempfile.mkdtemp(prefix="lspscale5352-")
            path = Path(root) / "syn.aura"
            path.write_text(text, encoding="utf-8")
            client = Client()
            try:
                client.initialize(caps={}, root=Path(root).as_uri())
                start = len(client.frames)
                client.did_open(path.as_uri(), text)
                _frames_after(client, start)
                t0 = time.perf_counter()
                client.send(
                    {
                        "jsonrpc": "2.0",
                        "id": 77,
                        "method": method,
                        "params": {"textDocument": {"uri": path.as_uri()}},
                    }
                )
                deadline = time.monotonic() + 20.0
                while time.monotonic() < deadline:
                    if any(f.get("id") == 77 for f in client.frames):
                        break
                    # A fine pump: a 50 ms slice would swamp the sub-10 ms
                    # requests the ratio is about.
                    client.pump(0.001)
                times.append((time.perf_counter() - t0) * 1000.0)
            finally:
                client.close()
        times.sort()
        return times[len(times) // 2]

    def test_symbol_queries_scale_with_the_file(self) -> None:
        """#5352: documentSymbol/foldingRange locate lines through LineIndex.

        range_json used to rescan the file from byte 0 for every symbol, so a
        4x larger file cost ~12-15x.  The bound is loose enough for a shared
        runner but rejects the quadratic shape (target is near-linear).
        """

        def gen(n: int) -> str:
            lines = ['(require "std/string" all:)', ""]
            for i in range(n):
                prev = f"(syn-f{i - 1} (+ x 1))" if i else "x"
                lines += [
                    f"; synthetic function {i}",
                    f"(define (syn-f{i} x)",
                    "  (let ((y (* x 2)))",
                    f"    (if (> y 100) y {prev})))",
                ]
            return "\n".join(lines) + "\n"

        small, big = gen(312), gen(1250)
        for method in ("textDocument/documentSymbol", "textDocument/foldingRange"):
            a = self._timed_symbol_request(small, method)
            b = self._timed_symbol_request(big, method)
            self.assertLessEqual(b / max(a, 1.0), 6.0, (method, a, b))

    def test_capabilities_advertise_the_symbol_layer(self) -> None:
        c = Client()
        try:
            caps = c.initialize()["capabilities"]
            self.assertTrue(caps["documentSymbolProvider"])
            self.assertTrue(caps["workspaceSymbolProvider"])
            self.assertTrue(caps["foldingRangeProvider"])
            self.assertTrue(caps["selectionRangeProvider"])
            self.assertTrue(caps["documentHighlightProvider"])
            self.assertEqual(caps["documentLinkProvider"]["resolveProvider"], False)
        finally:
            c.close()
