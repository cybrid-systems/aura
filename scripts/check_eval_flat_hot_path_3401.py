#!/usr/bin/env python3
"""Issue #3401: eval_flat hot-path intern — no try/catch in production, no heap push on happy path.

Contract:
  AC1 eval_flat production path has no function-scope try { around the TCO
     loop — try is wrapped with #ifndef NDEBUG / #endif so production
     (NDEBUG) skips it; Soft (debug) keeps it for friendly Diagnostics.
  AC2 LiteralString arm looks up by v.sym_id (string_intern_by_sym_.get);
     std::string construction + string_heap_.push_back happen only on
     the first encounter of a unique literal (#3457 dense SymId).
  AC3 :foo keyword Variable arm looks up by v.sym_id
     (keyword_intern_by_sym_.get); std::string construction +
     keyword_table_.push_back happen only on the first encounter of a
     unique keyword. keyword_table_ entries keep the leading ':' .
  AC4 eval_env.lookup call site uses std::string_view (no std::string
     construction on the hot path); Env::lookup already takes string_view.
  AC5 Evaluator class declares string_intern_by_sym_ and
     keyword_intern_by_sym_ near short_str_cache_ / keyword_table_.
  AC6 no tests/core/test_issue_3401.cpp (extends existing tests per #81934);
     no docs/design/3401-*.md (per #1655); no `classify_eval_value_tag`
     banned-call reintroduction (per #2616 invariant).
  AC7 source-cite #3401 + test/cmake/build gate; no design docs.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _strip_comments_and_strings(src: str) -> str:
    # Rough strip for gate (// and /* */ and "...") — not a full C++ lexer.
    out = re.sub(r"//[^\n]*", "", src)
    out = re.sub(r"/\*.*?\*/", "", out, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def _arm_keywords(arm_text: str, required: list[str]) -> list[str]:
    return [k for k in required if k not in arm_text]


def main() -> int:
    fails: list[str] = []

    eval_flat = _read("src/compiler/evaluator_eval_flat.cpp")
    evaluator_ixx = _read("src/compiler/evaluator.ixx")
    env_cpp = _read("src/compiler/evaluator_env.cpp")
    build = _read("build.py")
    flat_stripped = _strip_comments_and_strings(eval_flat)

    # AC1: function-scope try { in eval_flat must be wrapped with
    # #ifndef NDEBUG within ~30 lines of the metrics counter. Use the
    # metric counter as the anchor (unique to eval_flat) and scan
    # forward for the try { + #ifndef NDEBUG pattern.
    metric_pos = flat_stripped.find("hotpath_eval_flat_calls.fetch_add(1, std::memory_order_relaxed)")
    if metric_pos == -1:
        fails.append("AC1: eval_flat hotpath_eval_flat_calls.fetch_add not found")
    else:
        window = flat_stripped[metric_pos : metric_pos + 2000]
        if "try {" not in window:
            fails.append(
                "AC1: eval_flat function-scope try { not found within 2000 chars of hotpath_eval_flat_calls.fetch_add"
            )
        else:
            try_pos = window.find("try {")
            prefix = window[max(0, try_pos - 500) : try_pos]
            if "#ifndef NDEBUG" not in prefix:
                fails.append(
                    "AC1: eval_flat function-scope try { is not wrapped with "
                    "#ifndef NDEBUG (production path still pays try-table cost)"
                )

    # AC2: LiteralString arm — the arm is identified by the unique
    # // Issue #3401: happy-path string intern comment that we added
    # when fixing the hot path. Other LiteralString cases in the file
    # (eval_flat_apply_* helpers) do NOT carry this comment.
    literal_anchor = "// Issue #3401: happy-path string intern"
    literal_anchor_pos = eval_flat.find(literal_anchor)
    if literal_anchor_pos == -1:
        fails.append("AC2: LiteralString arm #3401 hot-path comment not found (fix not applied to eval_flat)")
    else:
        # Extract the surrounding arm: the #3401 anchor comment header
        # spans 5 lines (~300 chars), then 4 lines of intern lookups +
        # intern-once construction + push_back + cache write + return.
        # 1500 chars covers the whole arm with margin.
        arm_text = eval_flat[literal_anchor_pos : literal_anchor_pos + 1500]
        required = [
            "string_intern_by_sym_.get(v.sym_id)",
            "string_view raw_sv = p->resolve",
            "string_intern_by_sym_.set(v.sym_id",
            "std::string raw(raw_sv)",
        ]
        missing = _arm_keywords(arm_text, required)
        if missing:
            fails.append(
                f"AC2: LiteralString arm missing keywords: {missing} "
                "(hot path still constructs std::string / does not intern)"
            )

    # AC3: :foo keyword Variable arm — anchored by the unique
    # // Issue #3401: keyword O(1) intern comment.
    keyword_anchor = "// Issue #3401: keyword O(1) intern"
    keyword_anchor_pos = eval_flat.find(keyword_anchor)
    if keyword_anchor_pos == -1:
        fails.append("AC3: :foo keyword Variable arm #3401 hot-path comment not found (fix not applied to eval_flat)")
    else:
        arm_text = eval_flat[keyword_anchor_pos : keyword_anchor_pos + 1500]
        required = [
            "string_view name = p->resolve",
            "keyword_intern_by_sym_.get(v.sym_id)",
            "keyword_intern_by_sym_.set(v.sym_id",
        ]
        missing = _arm_keywords(arm_text, required)
        if missing:
            fails.append(
                f"AC3: :foo keyword Variable arm missing keywords: {missing} "
                "(hot path still constructs std::string / does O(n) scan)"
            )

    # AC4: eval_env.lookup call site uses std::string_view (no std::string).
    if "eval_env.lookup(std::string(name))" in eval_flat:
        fails.append(
            "AC4: eval_env.lookup call site still constructs std::string (per AC4 lookup takes SymId or string_view)"
        )
    # Env::lookup signature is already string_view (per #2616-era signature).
    if "Env::lookup(std::string_view n)" not in env_cpp:
        fails.append("AC4: Env::lookup signature does not take std::string_view (env.cpp regression)")

    # AC5: Evaluator class declares SymId intern maps near
    # short_str_cache_ / keyword_table_.
    if "string_intern_by_sym_;" not in evaluator_ixx:
        fails.append(
            "AC5: Evaluator class is missing string_intern_by_sym_ member "
            "(LiteralString hot path still hashes string_view)"
        )
    if "keyword_intern_by_sym_;" not in evaluator_ixx:
        fails.append(
            "AC5: Evaluator class is missing keyword_intern_by_sym_ member (:foo hot path still hashes string_view)"
        )

    # AC6: no tests/core/test_issue_3401.cpp (extends existing tests per
    # #81934); no docs/design/3401-*.md (per #1655); #2616 ban preserved.
    if (ROOT / "tests" / "core" / "test_issue_3401.cpp").is_file():
        fails.append("AC6: tests/core/test_issue_3401.cpp exists — must extend existing test per #81934")
    if list((ROOT / "docs" / "design").glob("3401-*.md")):
        fails.append("AC6: docs/design/3401-*.md exists — design docs banned per #1655")
    # The classify_eval_value_tag ban (per #2616) must NOT regress in
    # eval_flat.
    if re.search(r"\bclassify_eval_value_tag\s*\(", flat_stripped):
        fails.append("AC6: classify_eval_value_tag reintroduction in eval_flat (#2616 hot-path ban violated)")

    # AC7: source-cite #3401 + build gate; no design docs.
    if "#3401" not in eval_flat and "#3401" not in evaluator_ixx:
        fails.append("AC7: source-cite #3401 missing from eval_flat / evaluator.ixx")
    if "check_eval_flat_hot_path_3401" not in build:
        fails.append("AC7: build.py does not register check_eval_flat_hot_path_3401")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("PASS: #3401 eval_flat hot-path intern contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
