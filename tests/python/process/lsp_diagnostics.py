"""Diagnostics upgrade for aura lsp (#5262, #5346).

The server consumes the structured diagnostic (#5240) instead of re-parsing
typecheck output: one code (the stable kind ident the CLI and serve JSON faces
print), the kind's severity, the producer's message with the hint kept apart,
relatedInformation for the require chain, and a did-you-mean quickfix. These
cases drive the real binary over framed stdio and compare it with the sibling
faces of the same diagnostic.
"""

from __future__ import annotations

import json
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from lsp_protocol import AURA, ROOT, Client, env, last_publication

MEMQ = "(display (memq 1 (list 1 2)))\n"
ARITY = "(define (f x) x)\n(display (f))\n"
PARSE = "(display (+ 1 2)\n"
TYPE_MISMATCH = '(define (g y) (+ y "s"))\n'
# #5347: the '(' of `(define (b x)` is line 4. std/string.aura is 335 lines;
# splicing it in front used to report "opened at line 339".
PAREN_REQUIRE = '(require "std/string" all:)\n(define (a x) x)\n\n(define (b x)\n  (+ x 1)\n'
PAREN_PLAIN = "(define (a x) x)\n\n(define (b x)\n  (+ x 1)\n"
OPENED_AT = re.compile(r"opened at line (\d+), column (\d+)")
# #5346: a type warning earlier in the file used to be the only diagnostic.
MASK = '(define (g y) (+ y "s"))\n(define (h x) (memq x (list 1 2)))\n'
# Minimized aura-runner lib/thing.aura: an if-branch type warning, then a
# call with the wrong number of arguments further down.
IF_ARITY = "(define (g x)\n  (if x 1 #t))\n(define (f a) a)\n(f)\n"
# assq is bound at runtime (it reaches std/list and fails as assv), so the
# editor filter drops it. no-such-zz is unbound on both faces.
TWO_UNBOUND = '(define (g y) (+ y "s"))\n(display (memq 1 (list 1)))\n(display (no-such-zz 1))\n'
# A form the file runner actually evaluates, so --error-format=json emits
# the runtime diagnostic the static set must still contain.
MASK_RUN = '(define (g y) (+ y "s"))\n(display (memq 1 (list 1 2)))\n'


def opened_at(message: str) -> tuple[int, int]:
    found = OPENED_AT.search(message)
    assert found is not None, message
    return int(found.group(1)), int(found.group(2))


def assert_message_line_matches_range(test: unittest.TestCase, diag: dict) -> tuple[int, int]:
    """The message's 1-based line/column is the same point as the 0-based range."""
    line, col = opened_at(diag["message"])
    start = diag["range"]["start"]
    test.assertEqual(line, start["line"] + 1, diag)
    test.assertEqual(col, start["character"] + 1, diag)
    return line, col


def run_aura(args: list[str], cwd: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(AURA), *args], cwd=cwd, capture_output=True, text=True, env=env(), timeout=120)


def run_with_std(args: list[str], cwd: str) -> subprocess.CompletedProcess[str]:
    # Same search path the LSP client below is given, so a require of std/string
    # is the file the server splices in.
    e = env({"AURA_PATH": str(ROOT / "lib"), "AURA_SANDBOX": "off"})
    return subprocess.run([str(AURA), *args], cwd=cwd, capture_output=True, text=True, env=e, timeout=120)


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


def json_objects(text: str) -> list[dict]:
    return [json.loads(ln) for ln in text.splitlines() if ln.startswith("{")]


def typecheck_keys(stdout: str) -> list[tuple[str, int, int]]:
    """(code, 0-based line, 0-based column) from `aura --typecheck` text."""
    phrase = {
        "parse error": "ParseError",
        "unbound variable": "UnboundVariable",
        "arity mismatch": "ArityMismatch",
        "type error": "TypeError",
    }
    found = []
    for ln in stdout.splitlines():
        head, sep, rest = ln.partition(": ")
        if not sep:
            continue
        bits = head.split(":")
        if len(bits) != 2 or not bits[0].isdigit() or not bits[1].isdigit():
            continue
        kind, _, _msg = rest.partition(": ")
        code = phrase.get(kind)
        if code is None:
            continue
        found.append((code, int(bits[0]) - 1, int(bits[1]) - 1))
    return found


def lsp_keys(diags: list[dict]) -> set[tuple[str, int, int]]:
    keys = set()
    for dx in diags:
        start = dx["range"]["start"]
        keys.add((dx["code"], start["line"], start["character"]))
    return keys


def by_code(diags: list[dict], code: str) -> dict:
    matches = [dx for dx in diags if dx["code"] == code]
    assert len(matches) == 1, (code, diags)
    return matches[0]


def covered(text: str, dx: dict) -> str:
    line = text.splitlines()[dx["range"]["start"]["line"]]
    start = dx["range"]["start"]["character"]
    end = dx["range"]["end"]["character"]
    return line[start:end]


def drive(
    text: str,
    init_opts: dict | None = None,
    carets: tuple[tuple[int, int], ...] = (),
    wait: float = 3.0,
) -> tuple[list[dict], list[dict], dict[tuple[int, int], list[dict]], str, str]:
    """Open one file and return (push diagnostics, pull items, code actions, dir, uri)."""
    d = tempfile.mkdtemp(prefix="lspdiag5346-")
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
        c.pump(wait)
        c.clear()
        c.did_open(uri, text)
        frames = c.pump(wait)
        pub = last_publication(frames, uri)
        assert pub is not None, frames
        pulled = c.pull(uri)
        assert isinstance(pulled, dict), pulled
        actions: dict[tuple[int, int], list[dict]] = {}
        for n, (line, character) in enumerate(carets):
            req = 80 + n
            c.send(
                {
                    "jsonrpc": "2.0",
                    "id": req,
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
            reply = next((f for f in c.pump(wait) if f.get("id") == req), None)
            assert reply is not None, f"no codeAction reply for {line}:{character}"
            actions[(line, character)] = reply.get("result") or []
        return pub["diagnostics"], pulled["items"], actions, d, uri
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
        # #5364: the message carries the CLI's wording, not just the name.
        self.assertEqual(dx["message"], "unbound variable: memq")
        self.assertNotIn("did you mean", dx["message"])
        self.assertEqual(dx["data"]["suggestion"], "member")

        obj = last_json(run_aura(["--error-format=json", "a.aura"], d).stderr)
        self.assertEqual(obj["code"], dx["code"])
        # The JSON faces keep the producer's bare message; the LSP prefixes
        # the kind because lsp-mode shows the message alone.
        self.assertEqual(obj["message"], "memq")
        self.assertEqual(dx["message"], "unbound variable: " + obj["message"])
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
        self.assertEqual(served["message"], obj["message"])
        self.assertIn(dx["message"], served["msg"])
        self.assertEqual(served["span"]["start"], obj["span"]["start"])

    def test_unbound_arity_and_parse_errors_are_errors(self) -> None:
        for text, code, message in (
            (MEMQ, "UnboundVariable", "unbound variable: memq"),
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

    def _open_in(self, root: Path, name: str, text: str) -> tuple[list[dict], str]:
        path = root / name
        path.write_text(text)
        uri = path.as_uri()
        c = Client({"AURA_PATH": str(ROOT / "lib")})
        try:
            c.send(
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"capabilities": {}, "rootUri": root.as_uri()},
                }
            )
            c.pump(2.0)
            c.clear()
            c.did_open(uri, text)
            frames = c.pump(3.0)
            return frames, uri
        finally:
            c.close()

    def test_require_std_string_unclosed_line_matches_range_and_cli(self) -> None:
        # #5347. Range was already the file's line; the message counted
        # std/string's 335 lines on top. CLI parses the file alone and was
        # already right — the two faces must stay the same text.
        root = Path(tempfile.mkdtemp(prefix="lspparse5347-"))
        probe = root / "probe.aura"
        probe.write_text('(require "std/string" all:)\n(display 1)\n')
        proved = run_with_std(["probe.aura"], str(root))
        self.assertEqual(proved.returncode, 0, proved.stderr)
        self.assertIn("1", proved.stdout)
        frames, uri = self._open_in(root, "paren_min3.aura", PAREN_REQUIRE)
        pub = last_publication(frames, uri)
        self.assertIsNotNone(pub, frames)
        diags = pub["diagnostics"]
        self.assertEqual(len(diags), 1, diags)
        dx = diags[0]
        self.assertEqual(dx["code"], "ParseError")
        line, _col = assert_message_line_matches_range(self, dx)
        self.assertEqual(line, 4, dx)
        self.assertIn("unclosed list opened at line 4, column 1", dx["message"])
        std_lines = len((ROOT / "lib" / "std" / "string.aura").read_text().splitlines())
        self.assertNotIn(f"line {line + std_lines}", dx["message"])
        cli = run_with_std(["--error-format=json", "paren_min3.aura"], str(root))
        self.assertNotEqual(cli.returncode, 0, cli.stdout)
        obj = last_json(cli.stderr)
        self.assertEqual(obj["code"], "ParseError")
        self.assertEqual(obj["message"], dx["message"])
        self.assertEqual(obj["span"]["start"]["line"], line)
        text = run_with_std(["paren_min3.aura"], str(root))
        self.assertIn("unclosed list opened at line 4, column 1", text.stderr)
        self.assertIn(":4:1:", text.stderr)

    def test_unclosed_without_require_line_matches_range(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="lspparse5347b-"))
        frames, uri = self._open_in(root, "paren_plain.aura", PAREN_PLAIN)
        pub = last_publication(frames, uri)
        self.assertIsNotNone(pub, frames)
        diags = pub["diagnostics"]
        self.assertEqual(len(diags), 1, diags)
        dx = diags[0]
        self.assertEqual(dx["code"], "ParseError")
        line, _col = assert_message_line_matches_range(self, dx)
        self.assertEqual(line, 3, dx)
        cli = run_with_std(["--error-format=json", "paren_plain.aura"], str(root))
        self.assertEqual(last_json(cli.stderr)["message"], dx["message"])
        text = run_with_std(["paren_plain.aura"], str(root))
        self.assertIn("unclosed list opened at line 3, column 1", text.stderr)

    def test_unclosed_in_required_file_uses_that_files_line(self) -> None:
        # The required file is not the first segment: std/string is spliced
        # in front of it. The diagnostic stays on that file, numbered there.
        root = Path(tempfile.mkdtemp(prefix="lspparse5347c-"))
        dep = root / "dep.aura"
        dep.write_text('(require "std/string" all:)\n(define (broken x\n')
        frames, _uri = self._open_in(root, "main.aura", '(require "dep" all:)\n(define (ok x) x)\n')
        pub = last_publication(frames, dep.as_uri())
        self.assertIsNotNone(pub, frames)
        diags = pub["diagnostics"]
        self.assertEqual(len(diags), 1, diags)
        dx = diags[0]
        self.assertEqual(dx["code"], "ParseError")
        line, _col = assert_message_line_matches_range(self, dx)
        self.assertEqual(line, 2, dx)
        self.assertNotIn("line 337", dx["message"])
        cli = run_with_std(["--error-format=json", "dep.aura"], str(root))
        self.assertEqual(last_json(cli.stderr)["message"], dx["message"])

    def test_type_warning_does_not_mask_unbound_or_its_quickfix(self) -> None:
        # #5346 diag_mask.aura: the type warning is first, and memq still
        # publishes with a quickfix. Pull matches the push.
        diags, pulled, actions, d, uri = drive(MASK, carets=((1, 15),))
        self.assertEqual(pulled, diags)
        self.assertEqual(
            [(dx["code"], dx["severity"]) for dx in diags],
            [("TypeError", 2), ("UnboundVariable", 1)],
            diags,
        )
        warning, unbound = diags
        self.assertIn("expected Int", warning["message"])
        self.assertNotIn("type error:", warning["message"])
        self.assertEqual(
            warning["range"],
            {"start": {"line": 0, "character": 19}, "end": {"line": 0, "character": 20}},
        )
        self.assertEqual(unbound["message"], "unbound variable: memq")
        self.assertEqual(unbound["data"]["suggestion"], "member")
        self.assertEqual(
            unbound["range"],
            {"start": {"line": 1, "character": 15}, "end": {"line": 1, "character": 19}},
        )
        self.assertEqual(covered(MASK, unbound), "memq")
        fixes = actions[(1, 15)]
        self.assertEqual(len(fixes), 1, fixes)
        fix = fixes[0]
        self.assertEqual(fix["kind"], "quickfix")
        self.assertNotIn("command", fix)
        edit = fix["edit"]["changes"][uri][0]
        self.assertEqual(edit["newText"], "member")
        self.assertEqual(edit["range"], unbound["range"])

        tc = run_aura(["--typecheck", "a.aura"], d)
        self.assertNotEqual(tc.returncode, 0)
        static = typecheck_keys(tc.stdout)
        self.assertEqual(static, [("TypeError", 0, 19), ("UnboundVariable", 1, 15)], tc.stdout)
        published = lsp_keys(diags)
        for key in static:
            self.assertIn(key, published, (key, diags))

        # The file runner stops at the first error (#4909). Whatever it
        # emits, the LSP set contains it (equal, or a superset).
        ran = run_aura(["--error-format=json", "a.aura"], d)
        for obj in json_objects(ran.stderr):
            span = obj["span"]["start"]
            self.assertIn((obj["code"], span["line"] - 1, span["column"] - 1), published, obj)

        # A program that actually calls memq makes the JSON face non-empty.
        run_dir = tempfile.mkdtemp(prefix="lspdiag5346-run-")
        (Path(run_dir) / "a.aura").write_text(MASK_RUN)
        executed = run_aura(["--error-format=json", "a.aura"], run_dir)
        emitted = json_objects(executed.stderr)
        self.assertTrue(emitted, executed.stderr)
        live, _pulled, _actions, _live_dir, _live_uri = drive(MASK_RUN)
        live_keys = lsp_keys(live)
        self.assertIn(("TypeError", 2), [(dx["code"], dx["severity"]) for dx in live])
        self.assertIn(("UnboundVariable", 1), [(dx["code"], dx["severity"]) for dx in live])
        for obj in emitted:
            span = obj["span"]["start"]
            self.assertIn((obj["code"], span["line"] - 1, span["column"] - 1), live_keys, (obj, live))

    def test_type_warning_does_not_mask_arity_mismatch(self) -> None:
        # Minimized thing.aura: the if joins Int and Bool (a type warning),
        # then (f) is called with the wrong number of arguments.
        diags, pulled, _actions, d, _uri = drive(IF_ARITY)
        self.assertEqual(pulled, diags)
        warning = by_code(diags, "TypeError")
        arity = by_code(diags, "ArityMismatch")
        self.assertEqual(warning["severity"], 2, warning)
        self.assertIn("if branches join Int and Bool", warning["message"])
        # The join is blamed on the Bool branch, so the range covers #t.
        self.assertEqual(
            warning["range"],
            {"start": {"line": 1, "character": 10}, "end": {"line": 1, "character": 12}},
        )
        self.assertEqual(covered(IF_ARITY, warning), "#t")
        self.assertEqual(arity["severity"], 1, arity)
        self.assertIn("call 'f'", arity["message"])
        self.assertEqual(
            arity["range"],
            {"start": {"line": 3, "character": 1}, "end": {"line": 3, "character": 2}},
        )
        self.assertEqual(covered(IF_ARITY, arity), "f")
        starts = [(dx["range"]["start"]["line"], dx["range"]["start"]["character"]) for dx in diags]
        self.assertEqual(starts, sorted(starts), diags)
        self.assertLess(warning["range"]["start"]["line"], arity["range"]["start"]["line"])
        static = typecheck_keys(run_aura(["--typecheck", "a.aura"], d).stdout)
        published = lsp_keys(diags)
        for key in static:
            self.assertIn(key, published, (key, diags, static))

    def test_two_unbound_names_and_type_diagnostics_off(self) -> None:
        diags, _pulled, actions, _d, uri = drive(TWO_UNBOUND, carets=((1, 10), (2, 10)))
        self.assertEqual([dx["code"] for dx in diags], ["TypeError", "UnboundVariable", "UnboundVariable"])
        self.assertEqual([dx["severity"] for dx in diags], [2, 1, 1])
        self.assertEqual(
            [dx["message"] for dx in diags[1:]],
            ["unbound variable: memq", "unbound variable: no-such-zz"],
        )
        self.assertEqual(covered(TWO_UNBOUND, diags[1]), "memq")
        self.assertEqual(covered(TWO_UNBOUND, diags[2]), "no-such-zz")
        memq = actions[(1, 10)]
        other = actions[(2, 10)]
        self.assertEqual(len(memq), 1, memq)
        self.assertEqual(memq[0]["kind"], "quickfix")
        self.assertEqual(memq[0]["edit"]["changes"][uri][0]["newText"], "member")
        self.assertEqual(memq[0]["edit"]["changes"][uri][0]["range"], diags[1]["range"])
        # no-such-zz has no did-you-mean token, so it publishes with no quickfix.
        self.assertEqual(other, [])

        off, _pulled, off_actions, _d, off_uri = drive(TWO_UNBOUND, {"typeDiagnostics": "off"}, carets=((1, 10),))
        self.assertEqual([dx["code"] for dx in off], ["UnboundVariable", "UnboundVariable"], off)
        self.assertTrue(all(dx["severity"] == 1 for dx in off), off)
        self.assertEqual([dx["message"] for dx in off], ["unbound variable: memq", "unbound variable: no-such-zz"])
        self.assertEqual(off_actions[(1, 10)][0]["edit"]["changes"][off_uri][0]["newText"], "member")

    def test_per_file_cap_keeps_the_earliest_errors(self) -> None:
        # One type warning plus more arity errors than the per-file cap.
        # The published slice is the earliest 100 errors, still in order.
        lines = ['(define (g y) (+ y "s"))', "(define (f a) a)", *["(f)"] * 120]
        text = "\n".join(lines) + "\n"
        diags, pulled, _actions, _d, _uri = drive(text, wait=8.0)
        self.assertEqual(pulled, diags)
        self.assertEqual(len(diags), 100, [dx["code"] for dx in diags[:3]])
        self.assertTrue(all(dx["code"] == "ArityMismatch" and dx["severity"] == 1 for dx in diags), diags[0])
        starts = [(dx["range"]["start"]["line"], dx["range"]["start"]["character"]) for dx in diags]
        self.assertEqual(starts, sorted(starts))
        # Line 0 is the warning, line 1 is (define (f a) a), calls start at line 2.
        self.assertEqual(starts[0][0], 2)
        self.assertEqual(starts[-1][0], 101)
        self.assertNotIn("TypeError", [dx["code"] for dx in diags])


# #5362: the runtime and the JSON face report the call site for a wrong
# argument count; the LSP published nothing because the static diagnostic
# carried no location (an Int literal argument clobbered the checker's
# scratch location with {0, 0}, and the server drops line 0).
ARITY_NESTED = "(define (two a b) (+ a b))\n(display (two 1))\n"


class LspDiagnostics5362(unittest.TestCase):
    def test_arity_mismatch_in_a_nested_call_reports_the_call_site(self) -> None:
        diags, pulled, _actions, d, _uri = drive(ARITY_NESTED)
        self.assertEqual(pulled, diags)
        self.assertEqual([dx["code"] for dx in diags], ["ArityMismatch"], diags)
        arity = diags[0]
        self.assertEqual(arity["severity"], 1, arity)
        self.assertIn("call 'two'", arity["message"])
        self.assertIn("expected 2 arguments, got 1", arity["message"])
        # (two 1) is the second line; the range covers the callee, which is
        # where the runtime face points (2:11 there, 0-based here).
        self.assertEqual(arity["range"]["start"]["line"], 1, arity)
        self.assertIn(arity["range"]["start"]["character"], (9, 10), arity)
        self.assertEqual(covered(ARITY_NESTED, arity), "two")

        # Same point as the runtime JSON face.
        obj = last_json(run_aura(["--error-format=json", "a.aura"], d).stderr)
        self.assertEqual(obj["code"], "ArityMismatch")
        self.assertEqual(obj["span"]["start"]["line"] - 1, arity["range"]["start"]["line"])
        self.assertEqual(obj["span"]["start"]["column"] - 1, arity["range"]["start"]["character"])

        # The root cause was in the type checker, so --typecheck names the
        # point too (it used to print the message with no file:line:col).
        tc = run_aura(["--typecheck", "a.aura"], d)
        self.assertNotEqual(tc.returncode, 0)
        self.assertIn("2:11: arity mismatch: call 'two': expected 2 arguments, got 1", tc.stdout)
        self.assertEqual(typecheck_keys(tc.stdout), [("ArityMismatch", 1, 10)], tc.stdout)


# #5364 point 3: http-post is a deferred std host primitive, not a boot one
# (evaluator_module_loader.cpp: ensure_std_host_prims installs http-*/tcp-*
# when std/net, std/socket or std/llm loads). A file that requires none of
# them has no binding, so the diagnostic matches the runtime and stays.
HTTP_POST = '(define (post-it url body)\n  (try (http-post url body) (catch (e) "")))\n'


class LspDiagnostics5364(unittest.TestCase):
    def test_unbound_message_is_readable_and_the_filter_still_keys_on_the_name(self) -> None:
        diags, _d, _uri = open_diagnostics(MEMQ)
        self.assertEqual(len(diags), 1, diags)
        self.assertEqual(diags[0]["message"], "unbound variable: memq")
        self.assertIn("memq", diags[0]["message"])

        # The name filter is unchanged: assq is bound at runtime (it reaches
        # std/list), so it is dropped even though its message is now prefixed.
        filtered, _d, _uri = open_diagnostics("(display (assq 1 (list 1)))\n")
        self.assertEqual(filtered, [], filtered)

    def test_type_diagnostics_accepts_json_booleans(self) -> None:
        # (define (g y) (+ y "s")) is the type warning; the memq call in (h …)
        # is the error that must survive.
        off, _pulled, _actions, _d, _uri = drive(MASK, {"typeDiagnostics": False})
        self.assertEqual([dx["code"] for dx in off], ["UnboundVariable"], off)
        self.assertEqual(off[0]["severity"], 1, off[0])

        on, _pulled, _actions, _d, _uri = drive(MASK, {"typeDiagnostics": True})
        self.assertEqual([dx["code"] for dx in on], ["TypeError", "UnboundVariable"], on)
        self.assertEqual(on[0]["severity"], 2, on[0])

        # The same value over workspace/didChangeConfiguration.
        d = tempfile.mkdtemp(prefix="lspdiagbool5364-")
        path = Path(d) / "a.aura"
        path.write_text(MASK)
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
            c.did_open(uri, MASK)
            self.assertEqual(
                [dx["code"] for dx in last_publication(c.pump(2.0), uri)["diagnostics"]],
                ["TypeError", "UnboundVariable"],
            )
            c.clear()
            c.send(
                {
                    "jsonrpc": "2.0",
                    "method": "workspace/didChangeConfiguration",
                    "params": {"settings": {"typeDiagnostics": False}},
                }
            )
            self.assertEqual(
                [dx["code"] for dx in last_publication(c.pump(2.0), uri)["diagnostics"]],
                ["UnboundVariable"],
            )
        finally:
            c.close()

    def test_http_post_without_std_net_is_unbound_on_every_face(self) -> None:
        diags, _d, _uri = open_diagnostics(HTTP_POST)
        # The try/catch also earns a type warning about the joined branches;
        # the unbound name is the first (earliest) diagnostic.
        self.assertIn(("UnboundVariable", 1), [(dx["code"], dx["severity"]) for dx in diags], diags)
        self.assertEqual(diags[0]["code"], "UnboundVariable", diags)
        self.assertEqual(diags[0]["message"], "unbound variable: http-post")
        self.assertEqual(diags[0]["range"]["start"], {"line": 1, "character": 8})

        # The runtime agrees: not a false positive, so it is reported.
        probe = tempfile.mkdtemp(prefix="lspdiaghttp5364-")
        (Path(probe) / "b.aura").write_text('(display (http-post "http://127.0.0.1:1" ""))\n')
        ran = run_aura(["--error-format=json", "b.aura"], probe)
        self.assertNotEqual(ran.returncode, 0, ran.stdout)
        obj = last_json(ran.stderr)
        self.assertEqual(obj["code"], "UnboundVariable")
        self.assertEqual(obj["message"], "http-post")

        # …and it is bound once std/net loads, which is why the name exists at
        # all: the primitive is installed by the module, not at boot.
        root = Path(tempfile.mkdtemp(prefix="lspdiaghttp5364b-"))
        (root / "net.aura").write_text('(require "std/net" all:)\n(display (procedure? http-post))\n')
        loaded = run_with_std(["net.aura"], str(root))
        self.assertEqual(loaded.returncode, 0, loaded.stderr)
        self.assertIn("#t", loaded.stdout)
