"""Formatting cases for aura lsp (#5263).

textDocument/formatting and textDocument/rangeFormatting call the same C++
formatter as aura --fmt (cli_fmt_5263::reindent), so the editor and the CLI
must produce byte-identical text. These cases drive the real binary over
framed stdio and apply the TextEdits it returns.

Positions here are UTF-16 units on the wire; the corpus is ASCII plus BMP
Chinese, where a Python string index equals the wire character count.
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from urllib.parse import quote

from lsp_protocol import Client, env

ROOT = Path(__file__).resolve().parents[3]
AURA = ROOT / "build" / "aura"

# Badly indented on purpose: line comment, trailing comment, two-line block
# comment with Chinese text, the define sugar, a lambda the source itself
# wrote, and several top-level forms.
MESSY = r"""; 文件头注释：保留
(define (f x)
(+ x
1)) ; 行尾注释
#| 块注释
   跨行：保留 |#
(define g (lambda (y) (* y 2)))
(display (f 41))(display "\n")
(display (g 21))(display "\n")
"""
COMMENT_LINES = ("; 文件头注释：保留", "; 行尾注释", "#| 块注释", "跨行：保留 |#")
BROKEN = "(define (f x)\n  (+ x 1)\n"
# #5348: the head of a body form ends the line, so its clauses must all land
# at the same column. tabSize is a tab width, never the body indent.
COND_EOL = """(define (f i)
(cond
((= i 0) 0.3)
((= i 1) 0.7)
(else 1.1)))
"""


def uri_for(path: Path) -> str:
    return "file://" + quote(str(path))


def skeleton(text: str) -> str:
    return "\n".join(line.lstrip(" \t") for line in text.split("\n"))


def indent_columns(text: str, tab_size: int) -> list[int]:
    """Column each line starts at, with a tab advancing to the next stop."""
    columns = []
    for line in text.split("\n"):
        col = 0
        for ch in line:
            if ch == "\t":
                col = (col // tab_size + 1) * tab_size
            elif ch == " ":
                col += 1
            else:
                break
        columns.append(col)
    return columns


def offset(text: str, line: int, character: int) -> int:
    lines = text.split("\n")
    return sum(len(part) + 1 for part in lines[:line]) + character


def apply_edits(text: str, edits: list[dict]) -> str:
    out = text
    for edit in edits:
        rng = edit["range"]
        start = offset(out, rng["start"]["line"], rng["start"]["character"])
        end = offset(out, rng["end"]["line"], rng["end"]["character"])
        out = out[:start] + edit["newText"] + out[end:]
    return out


def run_file(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(AURA), str(path)], capture_output=True, text=True, env=env(), timeout=60)


class FmtClient(Client):
    """The protocol client plus the two formatting requests."""

    def format_document(self, uri: str, options: dict | None = None, rid: int = 40):
        return self._request(uri, None, options, rid)

    def format_range(self, uri: str, rng: dict, options: dict | None = None, rid: int = 41):
        return self._request(uri, rng, options, rid)

    def _request(self, uri: str, rng: dict | None, options: dict | None, rid: int):
        params: dict = {
            "textDocument": {"uri": uri},
            "options": options or {"tabSize": 2, "insertSpaces": True},
        }
        method = "textDocument/formatting"
        if rng is not None:
            method = "textDocument/rangeFormatting"
            params["range"] = rng
        first = len(self.frames)
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        for frame in self.pump(5.0)[first:]:
            if frame.get("id") == rid:
                return frame.get("result", frame.get("error"))
        return "no reply"


class LspFormat5263(unittest.TestCase):
    def _open(self, tmp: str, text: str) -> tuple[FmtClient, Path, str]:
        root = Path(tmp)
        path = root / "doc.aura"
        path.write_text(text, encoding="utf-8")
        client = FmtClient()
        caps = {"general": {"positionEncodings": ["utf-16"]}}
        init = client.initialize(caps=caps, root=uri_for(root))
        uri = uri_for(path)
        client.did_open(uri, text)
        client.pump(1.0)
        self.assertTrue(init["capabilities"]["documentFormattingProvider"])
        self.assertTrue(init["capabilities"]["documentRangeFormattingProvider"])
        return client, path, uri

    def test_formatting_returns_a_text_edit_and_keeps_comments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            client, path, uri = self._open(tmp, MESSY)
            result = client.format_document(uri)
            self.assertIsInstance(result, list, result)
            self.assertEqual(len(result), 1, result)
            edit = result[0]
            self.assertEqual(edit["range"]["start"], {"line": 0, "character": 0})
            applied = apply_edits(MESSY, result)
            for line in COMMENT_LINES:
                self.assertIn(line, applied)
            self.assertNotIn("(define f (lambda", applied)
            self.assertNotIn("(begin", applied)
            self.assertIn("\n  (+ x\n", applied)
            self.assertEqual(skeleton(applied), skeleton(MESSY))
            before = run_file(path)
            path.write_text(applied, encoding="utf-8")
            after = run_file(path)
            self.assertEqual(before.returncode, 0, before.stderr)
            self.assertEqual(after.stdout, before.stdout)
            client.close()

    def test_formatting_matches_cli_fmt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            client, path, uri = self._open(tmp, MESSY)
            applied = apply_edits(MESSY, client.format_document(uri))
            client.close()
            cli = subprocess.run(
                [str(AURA), "--fmt", "-i", str(path)],
                capture_output=True,
                text=True,
                env=env(),
                timeout=60,
            )
            self.assertEqual(cli.returncode, 0, cli.stderr)
            self.assertEqual(path.read_text(encoding="utf-8"), applied)

    def test_tab_size_is_not_the_body_indent(self) -> None:
        # #5348: FormattingOptions.tabSize is "size of a tab in spaces". The
        # formatter must keep the CLI's two-column body indent for every
        # tabSize, and insertSpaces = false must land on the same columns.
        cli = subprocess.run(
            [str(AURA), "--fmt"],
            input=COND_EOL,
            capture_output=True,
            text=True,
            env=env(),
            timeout=60,
        )
        self.assertEqual(cli.returncode, 0, cli.stderr)
        expected = cli.stdout
        self.assertIn("  (cond\n    ((= i 0) 0.3)\n", expected)
        for opts in (
            {"tabSize": 2, "insertSpaces": True},
            {"tabSize": 4, "insertSpaces": True},
            {"tabSize": 8, "insertSpaces": True},
            {"tabSize": 4, "insertSpaces": False},
            {"tabSize": 8, "insertSpaces": False},
        ):
            with tempfile.TemporaryDirectory() as tmp:
                client, _path, uri = self._open(tmp, COND_EOL)
                applied = apply_edits(COND_EOL, client.format_document(uri, opts))
                client.close()
            if opts["insertSpaces"]:
                self.assertEqual(applied, expected, opts)
            else:
                self.assertEqual(
                    indent_columns(applied, opts["tabSize"]),
                    indent_columns(expected, 2),
                    opts,
                )

    def test_formatting_is_idempotent_in_the_editor(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            client, _path, uri = self._open(tmp, MESSY)
            once = apply_edits(MESSY, client.format_document(uri))
            client.did_change(uri, once)
            client.pump(1.0)
            second = client.format_document(uri, rid=42)
            self.assertEqual(second, [], second)
            client.close()

    def test_range_formatting_keeps_other_lines(self) -> None:
        # Lines 2..3 are the body of (define (f x): format only those. Line 2
        # keeps its column-0 indent, the requested lines move in.
        with tempfile.TemporaryDirectory() as tmp:
            client, _path, uri = self._open(tmp, MESSY)
            rng = {"start": {"line": 2, "character": 0}, "end": {"line": 3, "character": 3}}
            result = client.format_range(uri, rng)
            self.assertIsInstance(result, list, result)
            self.assertEqual(len(result), 1, result)
            self.assertEqual(result[0]["range"]["start"], {"line": 2, "character": 0})
            # The replacement runs to the start of line 4: a line edit owns the
            # newline that ends its last line.
            self.assertEqual(result[0]["range"]["end"], {"line": 4, "character": 0})
            applied = apply_edits(MESSY, result)
            lines = applied.split("\n")
            self.assertEqual(lines[2], "  (+ x")
            self.assertEqual(lines[0], "; 文件头注释：保留")
            self.assertEqual(skeleton(applied), skeleton(MESSY))
            client.close()

    def test_unparsable_buffer_returns_no_edits(self) -> None:
        # The reader rejects it: the reply is an empty TextEdit list (never a
        # rewrite), one window/logMessage says why, and the server stays up.
        with tempfile.TemporaryDirectory() as tmp:
            client, _path, uri = self._open(tmp, BROKEN)
            result = client.format_document(uri)
            self.assertEqual(result, [], result)
            logs = [
                f["params"]["message"]
                for f in client.frames
                if f.get("method") == "window/logMessage" and isinstance(f.get("params"), dict)
            ]
            self.assertTrue(any("not formatting" in m for m in logs), logs)
            code, err = client.close()
            self.assertEqual(code, 0, err)

    def test_tab_size_and_insert_spaces_are_honored(self) -> None:
        # #5348: tabSize is the width of a tab, not the body indent — the text
        # is the same for every tabSize. insertSpaces = false writes tabs only
        # where an indent lands on a tab stop, so the columns never move.
        with tempfile.TemporaryDirectory() as tmp:
            client, _path, uri = self._open(tmp, MESSY)
            two = apply_edits(MESSY, client.format_document(uri, options={"tabSize": 2, "insertSpaces": True}))
            four = apply_edits(
                MESSY,
                client.format_document(uri, options={"tabSize": 4, "insertSpaces": True}, rid=42),
            )
            eight = apply_edits(
                MESSY,
                client.format_document(uri, options={"tabSize": 8, "insertSpaces": True}, rid=43),
            )
            self.assertEqual(four, two)
            self.assertEqual(eight, two)
            self.assertIn("\n  (+ x\n", two)
            tabs2 = apply_edits(
                MESSY,
                client.format_document(uri, options={"tabSize": 2, "insertSpaces": False}, rid=44),
            )
            self.assertIn("\n\t(+ x\n", tabs2)
            tabs4 = apply_edits(
                MESSY,
                client.format_document(uri, options={"tabSize": 4, "insertSpaces": False}, rid=45),
            )
            self.assertIn("\n  (+ x\n", tabs4)
            self.assertEqual(tabs4.count("\t\t"), 0, tabs4)
            client.close()


if __name__ == "__main__":
    unittest.main()
