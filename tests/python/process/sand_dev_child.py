#!/usr/bin/env python3
"""Regression for #5086: preset=dev must mutate a child workspace.

Workspace 0 mutate:rebind succeeds under --preset dev, but evo:trial's
child used to return cow-refused unless AURA_SANDBOX=off. The child
rewrite stays off the live worldline.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "python"))

from _aura_harness import AURA_BIN  # noqa: E402

PROG = """
(require "std/evo-core" all:)
(set-code "(define (score x) (* x 2))")
(eval-current)
(define root-rb (mutate :rebind "score" "(lambda (x) (* x 3))" "root"))
(eval-current)
(define c (evo:candidate "(lambda (x) (* x 9))" (hash "target" "score")))
(define t (evo:trial c
                     (hash "apply" (lambda (cand)
                                     (eq? (mutate :rebind "score"
                                                         (evo:candidate-genome cand)
                                                         "child") #t))
                           "evaluate" (lambda (cand)
                                        (hash "objectives" (hash "y" (score 5))
                                              "constraints" (hash)
                                              "behavior" (hash))))))
(display "ROOT ") (display root-rb) (newline)
(display "STATUS ") (display (hash-ref t "status")) (newline)
(display "ERR ") (display (hash-ref t "error" #f)) (newline)
(display "Y ") (display (evo:objective t "y")) (newline)
(display "LIVE ") (display (score 5)) (newline)
(display "UNCHANGED ") (display (hash-ref t "live-unchanged")) (newline)
"""


def env() -> dict[str, str]:
    out = dict(os.environ)
    # The bug only shows while the sandbox stays Restricted.
    out.pop("AURA_SANDBOX", None)
    out.pop("AURA_PIPELINE_STRICT", None)
    return out


TRIALS = """
(require "std/evo-core" all:)
(require "std/llm" all:)
(set-code "(define (rule-entry x) 0)")
(eval-current)

(define (show tag)
  (display tag)
  (display " ")
  (write (capability? "io-read"))
  (display " ")
  (write (try (getenv "HOME") (catch (e) e)))
  (newline))

(define (one k)
  (evo:trial
    (evo:candidate
      (string-append "(lambda (x) " (number->string k) ")")
      (hash "target" "rule-entry"))
    (hash "apply" evo:default-apply
          "evaluate" (lambda (c)
                       (hash "objectives" (hash "v" (rule-entry 0)))))))

(show "before")
(for-each one (list 1 2 3 4 5))
(show "after")
(define chat (try (llm:chat "sys" "ping" (hash "max_tokens" 8) 2)
                  (catch (e) e)))
(display "chat-hash ")
(display (hash? chat))
(newline)
(display "chat-error ")
(display (if (hash? chat) (hash-ref chat "error" "missing") chat))
(newline)
"""

CYCLES = """
(require "std/mutate" all:)
(set-code "(define (rule-entry x) 0)")
(eval-current)

(define (cycle)
  (let ((c (workspace :create "child")))
    (workspace :switch c)
    (mutate :rebind "rule-entry" "(lambda (x) 1)" "child")
    (workspace :switch 0)
    (workspace :discard c)))

(define (show tag)
  (display tag)
  (display " ")
  (write (capability? "io-read"))
  (display " ")
  (write (try (getenv "HOME") (catch (e) e)))
  (newline))

(show "before")
(for-each (lambda (k) (cycle)) (list 1 2 3 4 5))
(show "after")
"""


def run_dev(program: str, *args: str) -> subprocess.CompletedProcess[str]:
    face = env()
    for key in ("LLM_API_KEY", "LLM_BASE_URL", "LLM_MODEL", "LLM_TIMEOUT"):
        face.pop(key, None)
    with tempfile.TemporaryDirectory() as d:
        (Path(d) / "p.aura").write_text(program)
        return subprocess.run(
            [str(AURA_BIN), "--preset", "dev", "--quiet", *args, "p.aura"],
            cwd=d,
            capture_output=True,
            text=True,
            env=face,
            timeout=120,
        )


class DevChildCow(unittest.TestCase):
    def test_preset_dev_trial_mutates_child_not_live(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "p.aura").write_text(PROG)
            r = subprocess.run(
                [str(AURA_BIN), "--preset", "dev", "--quiet", "p.aura"],
                cwd=d,
                capture_output=True,
                text=True,
                env=env(),
                timeout=120,
            )
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            self.assertNotIn("cow-refused", r.stdout + r.stderr)
            self.assertIn("ROOT #t", r.stdout)
            self.assertIn("STATUS ok", r.stdout)
            self.assertIn("Y 45", r.stdout)
            self.assertIn("LIVE 15", r.stdout)
            self.assertIn("UNCHANGED #t", r.stdout)

    def test_trials_keep_io_read(self) -> None:
        r = run_dev(TRIALS, "--grant", "net")
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        self.assertIn("before #t", r.stdout)
        self.assertIn("after #t", r.stdout)
        self.assertNotIn("capability denied", r.stdout + r.stderr)
        self.assertIn("chat-hash #t", r.stdout)
        self.assertIn("chat-error empty", r.stdout)

    def test_child_rebind_cycles_keep_io_read(self) -> None:
        r = run_dev(CYCLES)
        self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
        self.assertIn("before #t", r.stdout)
        self.assertIn("after #t", r.stdout)
        self.assertNotIn("capability denied", r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
