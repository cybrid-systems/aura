#!/usr/bin/env python3
"""Regression for #4911: REPL hot reload / worldline commands and `aura lsp`.

Before: the REPL had no :load/:reload/:ws/:worldline/:restore (a changed
strategy meant re-running the whole program, regenerating its data), there was
no `aura repl` for a pipe-driven session, and no language server
(`aura lsp` was "cannot open program file 'lsp'").
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

V1 = """; strategy
(define generated 0)
(define (gen-prices)
  (set! generated (+ generated 1))
  (list 1 2 3))
(define prices (gen-prices))
(define (score p) (apply + p))
(define weight 1)
(display "top-level ran")
(newline)
"""

V2 = V1.replace("(define (score p) (apply + p))", "(define (score p)\n  (* 10 (apply + p)))").replace(
    "(define weight 1)", "(define weight 7)\n(define (bonus) 5)"
)


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_KEEP_GOING", "AURA_PATH"):
        e.pop(k, None)
    return e


class Repl(unittest.TestCase):
    def test_reload_keeps_data(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "backtest.aura"
            f.write_text(V1)
            p = subprocess.Popen(
                [str(AURA_BIN), "repl", str(f)],
                cwd=d,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env(),
            )
            assert p.stdin and p.stdout
            p.stdin.write("(score prices)\n:snapshot v1\n")
            p.stdin.flush()
            seen = []
            while True:
                line = p.stdout.readline()
                self.assertTrue(line, "REPL closed early: " + "".join(seen))
                seen.append(line)
                if line.startswith("snapshot "):
                    snap = line.split()[1]
                    break
            self.assertIn("6\n", seen)
            # Edit the file while the session is live, then :reload.
            f.write_text(V2)
            out, err = p.communicate(
                ":reload\n(score prices)\ngenerated\nweight\n(bonus)\n:ws\n:worldline\n"
                + f":restore {snap}\n(score prices)\nweight\n(quit)\n",
                timeout=120,
            )
            log = "".join(seen) + out
            self.assertEqual(p.returncode, 0, log + err)
            self.assertIn("loaded " + str(f) + ": 5 define(s)", log)
            self.assertEqual(log.count("top-level ran"), 1, log)  # not re-run by :reload
            self.assertIn("changed [score weight] added [bonus] unchanged 3", log)
            lines = [x.strip() for x in out.splitlines()]
            i = next(n for n, x in enumerate(lines) if x.startswith("reloaded "))
            # score is a plain REPL define (no set-code workspace): redefined in place
            self.assertIn("redefined in place: [score]", lines[i + 1])
            self.assertEqual(lines[i + 2 : i + 6], ["60", "1", "7", "5"], out + err)  # generated stays 1
            self.assertRegex(out, r"score\s+fn\s+.*backtest\.aura:7")
            self.assertRegex(out, r"prices\s+value\s+.*backtest\.aura:6")
            self.assertRegex(out, r"mutation-log: total \d+, committed \d+")
            self.assertIn("#2 snapshot " + snap + " v1", out)
            self.assertRegex(out, r"#3 reload .*backtest\.aura: changed \[score weight\] added \[bonus\]")
            # :restore puts score/weight back (bonus was added later and stays)
            self.assertEqual(lines[-3:], ["restored snapshot " + snap + ": [score weight]", "6", "1"], out + err)
            self.assertNotIn("error", err.replace("aura: repl", ""), err)

    def test_reload_required_module_snapshot_restore(self) -> None:
        # The kit shape: main requires a module; edit the module, `:reload lib.aura`
        # rebinds only what changed (data from main stays), :restore rolls back.
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / "lib.aura"
            lib.write_text("(define (score p) (apply + p))\n(define (unused) 1)\n")
            main = Path(d) / "main.aura"
            main.write_text(
                '(require "./lib")\n(define generated 0)\n'
                "(define prices (begin (set! generated (+ generated 1)) (list 1 2 3)))\n"
            )
            p = subprocess.Popen(
                [str(AURA_BIN), "repl", str(main)],
                cwd=d,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env(),
            )
            assert p.stdin and p.stdout
            p.stdin.write("(score prices)\n")
            p.stdin.flush()
            seen = []
            while True:
                line = p.stdout.readline()
                self.assertTrue(line, "REPL closed early: " + "".join(seen))
                seen.append(line)
                if line.strip() == "6":
                    break
            lib.write_text("(define (score p)\n  (* 10 (apply + p)))\n(define (unused) 1)\n")
            out, err = p.communicate(":reload lib.aura\n(score prices)\ngenerated\n:worldline\n(quit)\n", timeout=120)
            self.assertEqual(p.returncode, 0, out + err)
            self.assertIn("changed [score] added [-] unchanged 1", out, out + err)
            lines = [x.strip() for x in out.splitlines()]
            i = next(n for n, x in enumerate(lines) if x.startswith("reloaded "))
            self.assertIn("redefined in place: [score]", lines[i + 1])
            self.assertEqual(lines[i + 2 : i + 4], ["60", "1"], out + err)  # prices not regenerated
            self.assertRegex(out, r"#1 load .*main\.aura")
            self.assertRegex(out, r"#2 reload .*lib\.aura: changed \[score\]")
            self.assertNotIn("error", err.replace("aura: repl", ""), err)

    def test_reload_required_module_then_workspace_snapshot_restore(self) -> None:
        # Kit shape with a live workspace (set-code): `:reload lib.aura` used
        # mutate:rebind on the required define, which answers #t outside the
        # workspace; the next ast:snapshot/rebind/ast:restore + eval-current
        # aborted in lower_flat_expr (evolve! after :reload backtest.aura).
        with tempfile.TemporaryDirectory() as d:
            lib = Path(d) / "lib.aura"
            lib.write_text(
                "(define (has? xs r) (if (member r xs) #t #f))\n"
                "(define (score ps rules) (+ (apply + ps) (if (has? rules 'b) 100 0)))\n"
            )
            main = Path(d) / "main.aura"
            main.write_text(
                '(require "./lib")\n'
                '(set-code "(define (params) (list 1 2))\\n(define (rules) (list \'a))\\n")\n'
                "(eval-current)\n"
                "(define (step! k)\n"
                '  (let* ((snap (ast:snapshot "s"))\n'
                '         (a (mutate:rebind "params" (format "(lambda () (list ~a 4))" k) "step"))\n'
                '         (b (mutate:rebind "rules" "(lambda () (list \'a \'b))" "step")))\n'
                "    (eval-current)\n"
                "    (let ((s (score (params) (rules))))\n"
                "      (ast:restore snap)\n"
                "      (eval-current)\n"
                "      (list a b s (score (params) (rules))))))\n"
            )
            p = subprocess.Popen(
                [str(AURA_BIN), "repl", str(main)],
                cwd=d,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env(),
            )
            assert p.stdin and p.stdout
            p.stdin.write("(score (params) (rules))\n")
            p.stdin.flush()
            seen = []
            while True:
                line = p.stdout.readline()
                self.assertTrue(line, "REPL closed early: " + "".join(seen))
                seen.append(line)
                if line.strip() == "3":
                    break
            lib.write_text(lib.read_text().replace("(if (member r xs) #t #f)", "(and (member r xs) #t)"))
            out, err = p.communicate(":reload lib.aura\n(step! 3)\n(step! 5)\n(quit)\n", timeout=120)
            self.assertEqual(p.returncode, 0, out + err)
            self.assertIn("changed [has?] added [-] unchanged 1", out, out + err)
            self.assertIn("redefined in place: [has?]", out, out + err)
            lines = [x.strip() for x in out.splitlines()]
            self.assertEqual(lines[-2:], ["(#t #t 107 3)", "(#t #t 109 3)"], out + err)

    def test_reload_without_load_and_help(self) -> None:
        r = subprocess.run(
            [str(AURA_BIN), "repl"],
            input=":reload\n:help\n(+ 1 2)\n(cdr (assq 1 (list (cons 1 2))))\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(":reload: no file loaded yet", r.stderr)
        self.assertIn(":worldline", r.stdout)
        self.assertIn("3", r.stdout.splitlines())
        self.assertIn("2", r.stdout.splitlines(), r.stdout + r.stderr)  # std prelude (assq) loaded

    def test_closures_after_set_code_and_heap_churn(self) -> None:
        # Kit shape (set-code, then data generation, then map over a closure)
        # under the default soft session: map returned (() ()) (#4928) and the
        # kit failed with "string-append … got Void" in the REPL only.
        prog = (
            '(set-code "(define (s) 1)")\n(define r (list 1))\n'
            "(define (bump n) (let loop ((i 0)) (if (< i n) (begin (set-car! r i) (loop (+ i 1))) (car r))))\n"
            "(bump 2000)\n(map (lambda (x) (+ x 1)) (list 1 2))\n"
        )
        e = env()
        e.pop("AURA_ARENA_MOVING_COMPACT", None)
        r = subprocess.run([str(AURA_BIN), "repl"], input=prog, capture_output=True, text=True, env=e, timeout=120)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip().splitlines()[-1], "(2 3)", r.stdout + r.stderr)


def frame(obj: dict) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def read_frames(raw: bytes) -> list[dict]:
    out = []
    while raw:
        head, _, rest = raw.partition(b"\r\n\r\n")
        n = int(head.split(b":")[1])
        out.append(json.loads(rest[:n]))
        raw = rest[n:]
    return out


class Lsp(unittest.TestCase):
    def test_diagnostics_and_definition_across_require(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d).resolve()
            (root / "aura.toml").write_text('[project]\nname = "kit"\n')
            (root / "lib").mkdir()
            # forward reference (helper), runtime/std names (format, assq, mutate:rebind): not diagnostics
            (root / "lib" / "util.aura").write_text(
                "(define (pick x xs)\n  (helper x xs))\n(define (helper x xs) (member x xs))\n"
                # optional third argument of a runtime primitive: not an arity diagnostic
                '(define (bump!) (mutate:rebind "pick" "(lambda (x xs) x)" "why"))\n'
            )
            main = root / "backtest.aura"
            main.write_text(
                '(require "lib/util")\n(define (has? x xs)\n  (memq x xs))\n(define r (pick 1 (list 1 2)))\n'
                '(display (format "~a ~a" r (assq 1 (list (cons 1 2)))))\n'
            )
            uri = main.as_uri()
            text = main.read_text()
            msgs = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": root.as_uri()}},
                {"jsonrpc": "2.0", "method": "initialized", "params": {}},
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {"textDocument": {"uri": uri, "languageId": "aura", "version": 1, "text": text}},
                },
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "textDocument/definition",
                    "params": {"textDocument": {"uri": uri}, "position": {"line": 3, "character": 12}},
                },
                {"jsonrpc": "2.0", "id": 3, "method": "shutdown"},
                {"jsonrpc": "2.0", "method": "exit"},
            ]
            r = subprocess.run(
                [str(AURA_BIN), "lsp"],
                input=b"".join(frame(m) for m in msgs),
                capture_output=True,
                env=env(),
                timeout=120,
            )
            self.assertEqual(r.returncode, 0, r.stderr.decode())
            frames = read_frames(r.stdout)
            init = next(f for f in frames if f.get("id") == 1)
            self.assertTrue(init["result"]["capabilities"]["definitionProvider"])
            diags = [f["params"] for f in frames if f.get("method") == "textDocument/publishDiagnostics"]
            mine = next(p for p in diags if p["uri"] == uri)["diagnostics"]
            self.assertEqual(len(mine), 1, mine)
            self.assertIn("memq", mine[0]["message"])
            self.assertEqual(mine[0]["range"]["start"], {"line": 2, "character": 3})
            self.assertEqual(mine[0]["range"]["end"], {"line": 2, "character": 7})
            # `pick` from the required module is not "unbound".
            self.assertFalse(any("pick" in x["message"] for p in diags for x in p["diagnostics"]), diags)
            self.assertEqual([p["diagnostics"] for p in diags if p["uri"] != uri], [[]] * (len(diags) - 1), diags)
            loc = next(f for f in frames if f.get("id") == 2)["result"]
            self.assertEqual(loc["uri"], (root / "lib" / "util.aura").as_uri())
            self.assertEqual(loc["range"]["start"], {"line": 0, "character": 9})

    def test_error_in_required_module_is_reported_there(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d).resolve()
            (root / "b.aura").write_text("(define (bfun x)\n  (undefined-thing x))\n")
            main = root / "m.aura"
            main.write_text('(require "b")\n(define z (bfun 1))\n')
            msgs = [
                {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": main.as_uri(),
                            "languageId": "aura",
                            "version": 1,
                            "text": main.read_text(),
                        }
                    },
                },
                {"jsonrpc": "2.0", "id": 9, "method": "shutdown"},
                {"jsonrpc": "2.0", "method": "exit"},
            ]
            r = subprocess.run(
                [str(AURA_BIN), "lsp"],
                input=b"".join(frame(m) for m in msgs),
                capture_output=True,
                env=env(),
                timeout=120,
            )
            self.assertEqual(r.returncode, 0, r.stderr.decode())
            diags = {
                f["params"]["uri"]: f["params"]["diagnostics"]
                for f in read_frames(r.stdout)
                if f.get("method") == "textDocument/publishDiagnostics"
            }
            self.assertEqual(diags[main.as_uri()], [])
            b = diags[(root / "b.aura").as_uri()]
            self.assertEqual(len(b), 1, b)
            self.assertIn("undefined-thing", b[0]["message"])
            self.assertEqual(b[0]["range"]["start"], {"line": 1, "character": 3})


class TypecheckStdin(unittest.TestCase):
    def test_multiline_stdin(self) -> None:
        # Before: --typecheck read only the first stdin line ("unclosed list").
        r = subprocess.run(
            [str(AURA_BIN), "--typecheck"],
            input="(define (f x)\n  (+ x 1))\n",
            capture_output=True,
            text=True,
            env=env(),
            timeout=120,
        )
        self.assertNotIn("unclosed", r.stdout + r.stderr)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
