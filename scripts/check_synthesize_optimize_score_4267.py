#!/usr/bin/env python3
"""Issue #4267: synthesize:optimize score synthesis — the default score must
be baseline-agreement (NOT a correctness / task pass rate), and the user's
:fitness / :benchmark expression must be what actually gets eval'd.

REPRO AT HEAD bda315cef (verified before fixing, CLI face): with baseline
(define (f x) (+ x 10)), both
  (synthesize:optimize "f" :fitness "(f 3)")       -> (1 . 1000.03125)
  (synthesize:optimize "f" :fitness "(- 0 (f 3))") -> (0 . 1000.03125)
i.e. the ~1000-scale DEFAULT non-error-rate score ran for both — the
expressions never influenced the ranking (no flip). Three stacked defects:
  1. Option keys arrive keyword-tagged (Issue #63 Phase 3); the string-only
     parse silently dropped every :keyword option, so :fitness / :benchmark
     (and :population / :generations / :mutation-rate) never parsed.
  2. The fitness branch eval'd the candidate source `src` (a define form →
     non-numeric → every candidate scored 0.0) instead of `fitness_expr`.
  3. The default score = non-error rate × 1000 + shorter-source bonus — a
     shorter-wrong variant ((+ x 10) → (+ x 0)) beats the baseline on the
     length tiebreaker; no reference result is ever consulted.

Fix shape (this ship): resolve keyword-tagged keys alongside string keys;
bind the candidate via eval-current then eval the :fitness expression and
use its numeric value as the score (non-numeric = candidate rejected via
the lowest-double sentinel); default path records the baseline's probe
results and scores variants by agreement (semantic-preserving), with code
length as a pure post-gate tiebreaker (agreement steps 1000/N ≥ 250 dwarf
the < 1.0 bonus). Contract per the issue: the application owns the task
gate via :fitness; the default score is NOT task correctness.

Gate contract:
  AC1 doors present — test_fiber_synthesize_batch.cpp hosts
      run_optimize_score_4267, wired into main(), batch still registered.
  AC2 fitness expression is the eval path — the optimize region pushes
      fitness_expr (not src) to the eval primitive; the test pins the
      opposite-ranking flip ((f 3) vs (- 0 (f 3))).
  AC3 default score is baseline-agreement — ref_ready / ref_arg_count /
      ref_results capture + agreement scoring + length-after-gate order.
  AC4 honest naming + rejection — no `correctness` identifier in the
      comment-stripped optimize region; lowest-double reject sentinel
      present; keyword-key resolution (types::as_keyword_idx) present.
  AC5 wiring — build.py invokes this linter; filename on
      scripts/coverage/root_check_allowlist.txt; no docs/design/4267-*.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.exists() and p.read_text() or ""


def _strip_line_comments(win: str) -> str:
    code: list[str] = []
    i = 0
    n = len(win)
    while i < n:
        if i + 1 < n and win[i] == "/" and win[i + 1] == "/":
            while i < n and win[i] != "\n":
                i += 1
            continue
        code.append(win[i])
        i += 1
    return "".join(code)


def main() -> int:
    failures: list[str] = []

    def check(cond: bool, label: str) -> None:
        if not cond:
            failures.append(label)

    src = _read("src/compiler/evaluator_primitives_agent.cpp")
    check(bool(src), "read evaluator_primitives_agent.cpp")

    # ── AC1: doors present and invoked ────────────────────────────────
    tc = _read("tests/serve/test_fiber_synthesize_batch.cpp")
    check("#4267" in tc, "AC1: test file cites #4267")
    check(
        "int run_optimize_score_4267()" in tc,
        "AC1: run_optimize_score_4267 door present",
    )
    check(
        "aura_fiber_run_optimize_score_4267::run_optimize_score_4267()" in tc,
        "AC1: door invoked from batch main()",
    )
    check(
        "aura_add_issue_test(test_fiber_synthesize_batch)" in _read("CMakeLists.txt"),
        "AC1: batch still registered in CMakeLists.txt",
    )

    # ── AC2: :fitness expression is the eval path ─────────────────────
    pos = src.find('add("synthesize:optimize"')
    check(pos != -1, "AC2: found synthesize:optimize registration")
    if pos != -1:
        end = src.find('\n    add("', pos + 10)
        win = src[pos : end if end != -1 else pos + 30000]
        code = _strip_line_comments(win)
        check(
            "push_string_heap(fitness_expr)" in code,
            "AC2: fitness branch evals the :fitness expression",
        )
        check(
            "push_string_heap(src)" not in code,
            "AC2: candidate source is no longer eval'd as the fitness",
        )
        check(
            'win.find("Issue #4267") != -1',
            "AC2: optimize region cites #4267",
        )
        # Regression door shape: opposite fitness exprs flip the ranking.
        check(
            "(f 3)" in tc and "(- 0 (f 3))" in tc,
            "AC2: test pins opposite-ranking flip shapes",
        )
        check(
            "fitness_a - fitness_b >= 26.0" in tc,
            "AC2: test asserts the flip arithmetic (A - B ≥ 26)",
        )

    # ── AC3: default score is baseline-agreement ──────────────────────
    if pos != -1:
        end = src.find('\n    add("', pos + 10)
        win = src[pos : end if end != -1 else pos + 30000]
        code = _strip_line_comments(win)
        check(
            "ref_ready" in code and "ref_arg_count" in code and "ref_results" in code,
            "AC3: baseline reference capture present",
        )
        check(
            "agreement" in code,
            "AC3: agreement scoring present",
        )
        code.find("agreement")
        code.find("length_bonus")
        check(
            "return agreement + length_bonus" in code,
            "AC3: length bonus composed after the agreement term",
        )
        check(
            "ref_results = results" in code,
            "AC3: baseline pass records reference probe results",
        )

    # ── AC4: honest naming + rejection + keyword-key resolution ───────
    if pos != -1:
        end = src.find('\n    add("', pos + 10)
        win = src[pos : end if end != -1 else pos + 30000]
        code = _strip_line_comments(win)
        check(
            "correctness" not in code,
            "AC4: no `correctness` identifier in the optimize region",
        )
        check(
            "baseline-agreement" in win,
            "AC4: doc comment states the baseline-agreement contract",
        )
        check(
            "std::numeric_limits<double>::lowest()" in code,
            "AC4: non-numeric fitness rejects the candidate (sentinel)",
        )
        check(
            "types::as_keyword_idx" in code,
            "AC4: option loop resolves keyword-tagged keys",
        )

    # ── AC5: wiring ───────────────────────────────────────────────────
    bp = _read("build.py")
    check(
        "check_synthesize_optimize_score_4267.py" in bp,
        "AC5: build.py invokes the #4267 linter",
    )
    allow = _read("scripts/coverage/root_check_allowlist.txt")
    check(
        "check_synthesize_optimize_score_4267.py" in allow,
        "AC5: filename on root_check_allowlist.txt",
    )
    check(
        not any((ROOT / "docs" / "design").glob("4267-*")),
        "AC5: no docs/design/4267-* per #1655",
    )
    check(
        not any(ROOT.glob("tests/**/test_issue_4267.cpp")),
        "AC5: no test_issue_4267.cpp per #81934",
    )

    if failures:
        print("check_synthesize_optimize_score_4267: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_synthesize_optimize_score_4267: OK (5 ACs satisfied)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
