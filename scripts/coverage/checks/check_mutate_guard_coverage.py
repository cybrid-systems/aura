#!/usr/bin/env python3
"""Issue #2986: every production mutate:* is Guard-wrapped or GUARD_EXEMPT.

Contract (one row per AC):
  AC1 Every production mutate:* registration is MutationBoundaryGuard
     wrapped (add_mutate / try_acquire / run_under_mutation_guard /
     TransactionGuard) or explicitly `// GUARD_EXEMPT: ` + PrimMeta flag.
  AC2 Linter fails CI on a new naked add("mutate:foo") without exemption.
     Self-test fixture mutate:__test_naked_2986 is never registered in src/.
  AC3 Existing mutate_guard_enforced / naked_mutate_attempt remain;
     production + naked attempt → fail-closed (counter + mark-failed).
  AC4 Metadata-only / policy setters stay GUARD_EXEMPT.
  AC5 Zero extra stores on the happy Guard path (fail-closed only on naked).
  AC6 Source-cite only; no docs/design/* per #1655.

Exit 0 = all rows satisfied. Exit 1 = naked mutate or missing contract row.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

ROOT = Path(__file__).resolve().parents[3]

# Production registration of mutate:* / canonical 6-op dispatcher.
# `\s*` spans the common
#   add_mutate(
#       "mutate:foo"
# split so AC1 cannot miss a multi-line registration.
ADD_RE = re.compile(
    r'(add_mutate|add|register_stats_impl)\(\s*"(mutate(?::[^"]+)?)"',
    re.MULTILINE,
)

WRAP_NEEDLES = (
    "MutationBoundaryGuard::try_acquire",
    "run_under_mutation_guard(",
    "run_compile_dirty_under_guard(",
    "TransactionGuard",
    "transaction_guard_host(",
    "make_transaction_guard(",
)

# Canonical 6-op surface (Issue #1436 / #2986).
CANONICAL_6 = (
    ("rebind", "mutate:rebind"),
    ("replace", "mutate:replace-subtree"),
    ("move", "mutate:move-node"),
    ("extract", "mutate:extract-function"),
    ("validate", "mutate:validate-against-schema"),
    ("atomic", "mutate:atomic-batch"),
)

NAKED_FIXTURE_NAME = "mutate:__test_naked_2986"

SCAN_GLOBS = (
    "evaluator_primitives_mutate.cpp",
    "evaluator_primitives_mutation*.cpp",
    "evaluator_primitives_*.cpp",
)

# Metadata / policy setters that must remain GUARD_EXEMPT (AC4).
REQUIRED_EXEMPT = (
    "mutate:set-agent-fingerprint",
    "mutate:set-stale-ref-policy",
    "mutate:save-hygiene-checkpoint",
    "mutate:check-stable-ref",
    "mutate:set-pattern-index-policy",
    "mutate:request-gc-safepoint",
    "mutate:validate-reflected",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def _scan_paths() -> list[Path]:
    seen: set[Path] = set()
    out: list[Path] = []
    base = ROOT / "src" / "compiler"
    for glob in SCAN_GLOBS:
        for p in sorted(base.glob(glob)):
            if p in seen:
                continue
            seen.add(p)
            out.append(p)
    return out


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def classify_text(text: str) -> list[tuple[int, str, str, str]]:
    """Return (lineno, kind, name, status) for each mutate registration.

    status is one of: wrapped, exempt, naked.
    """
    lines = text.splitlines()
    matches = list(ADD_RE.finditer(text))
    rows: list[tuple[int, str, str, str]] = []
    for idx, m in enumerate(matches):
        # Skip matches that live in a comment line.
        lineno = _line_of(text, m.start())
        line = lines[lineno - 1] if 0 < lineno <= len(lines) else ""
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        kind, name = m.group(1), m.group(2)
        i = lineno - 1
        pre = "\n".join(lines[max(0, i - 25) : i])
        nxt = _line_of(text, matches[idx + 1].start()) - 1 if idx + 1 < len(matches) else len(lines)
        window = "\n".join(lines[i:nxt])
        post = "\n".join(lines[i : min(len(lines), i + 80)])
        has_exempt = "GUARD_EXEMPT:" in pre or "GUARD_EXEMPT:" in window[:800]
        has_flag = "guard_exempt" in pre or "guard_exempt" in post
        if kind == "add_mutate":
            status = ("exempt" if has_flag else "naked") if has_exempt else "wrapped"
        elif any(n in window for n in WRAP_NEEDLES):
            status = "wrapped"
        elif has_exempt and has_flag:
            status = "exempt"
        else:
            status = "naked"
        rows.append((lineno, kind, name, status))
    return rows


def self_test_naked_fixture() -> str | None:
    """AC2: a deliberately-naked add(\"mutate:foo\") must classify as naked."""
    snippet = (
        "// production never registers this fixture\n"
        f'    add("{NAKED_FIXTURE_NAME}", [&ev](std::span<const EvalValue> a) -> EvalValue {{\n'
        "        return make_bool(true);\n"
        "    });\n"
    )
    rows = classify_text(snippet)
    if not rows:
        return "self-test: fixture produced no registrations"
    _, _, name, status = rows[0]
    if name != NAKED_FIXTURE_NAME:
        return f"self-test: expected {NAKED_FIXTURE_NAME}, got {name}"
    if status != "naked":
        return f"self-test: expected naked, got {status}"
    return None


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_key(n: str, label: str, hay: str) -> None:
        normalized = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
        if n not in normalized:
            fails.append(f"{label}: missing {n!r}")

    mut = _read("src/compiler/evaluator_primitives_mutate.cpp")
    qtail = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    obs = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    hh = _read("src/compiler/evaluator.ixx")
    metrics = _read("src/compiler/observability_metrics.h")
    t = _read("tests/compiler/test_mutation_guard_try_acquire_unit.cpp")
    build = _read("build.py")
    q = read_query_prims() + "\n" + obs + "\n" + compile_cpp

    # AC1 — scan every production mutate:* / 6-op dispatcher.
    found: dict[str, str] = {}
    for path in _scan_paths():
        rel = str(path.relative_to(ROOT))
        text = path.read_text(encoding="utf-8", errors="replace")
        if NAKED_FIXTURE_NAME in text:
            fails.append(f"AC2: production {rel} registers {NAKED_FIXTURE_NAME}")
        for lineno, kind, name, status in classify_text(text):
            found[name] = status
            if status == "naked":
                fails.append(f'AC1/AC2: naked {kind}("{name}") at {rel}:{lineno}')

    for op, prim in CANONICAL_6:
        must(f":{op}", "AC1 6-op dispatcher", mut)
        must(f'"{prim}"', f"AC1 6-op {op}", mut)
        if prim == "mutate:validate-against-schema":
            # stats_impl + dispatcher; must be exempt or wrapped.
            if (
                found.get(prim) not in {"exempt", "wrapped"}
                and prim not in found
                and ("GUARD_EXEMPT:" not in qtail or prim not in qtail)
            ):
                fails.append(f"AC1: 6-op :validate target {prim} missing wrap/exempt")
        elif found.get(prim) not in {"wrapped", "exempt"}:
            fails.append(f"AC1: 6-op :{op} target {prim} status={found.get(prim)!r}")

    must("guard_exempt", "AC1 PrimMeta flag", hh)
    must("Issue #2986", "AC1 add_mutate", mut)

    # AC2 — linter catches a deliberately-naked test prim.
    st = self_test_naked_fixture()
    if st:
        fails.append(f"AC2: {st}")
    must("ac2986_2_linter_catches_naked", "AC2", t)
    must(NAKED_FIXTURE_NAME, "AC2 fixture name in test", t)

    # AC3 — existing metrics + production fail-closed.
    must("naked_mutate_attempt", "AC3", mut)
    must("mutate_guard_enforced", "AC3", mut)
    must("naked_mutate_fail_closed_total", "AC3", metrics)
    must("naked_mutate_fail_closed_total", "AC3 add_mutate", mut)
    must("production_defaults_active()", "AC3", mut)
    must("mark_outermost_mutation_failed", "AC3", mut)
    must_key("schema-2986", "AC3", q)
    must_key("issue-2986", "AC3", q)
    must_key("naked-mutate-fail-closed-total", "AC3", q)
    must_key("naked-mutate-attempt", "AC3 preserved", q)
    must("ac2986_3_metrics_and_fail_closed", "AC3", t)

    # AC4 — metadata/policy remain exempt.
    for name in REQUIRED_EXEMPT:
        if found.get(name) != "exempt":
            fails.append(f"AC4: {name} status={found.get(name)!r} (want exempt)")
        hay = qtail if name == "mutate:validate-reflected" else mut
        if "GUARD_EXEMPT:" not in hay:
            fails.append(f"AC4: {name} file missing GUARD_EXEMPT comment")
    must("ac2986_4_exempt_policy_setters", "AC4", t)

    # AC5 — happy path: fail-closed only after wraps_after == wraps_before.
    must("wraps_after == wraps_before", "AC5", mut)
    must("Happy Guard path", "AC5", mut)
    must("ac2986_5_zero_happy_overhead", "AC5", t)
    must("check_mutate_guard_coverage", "AC5", build)

    # AC6 — no design doc / invent test.
    must("ac2986_6_source_and_linter", "AC6", t)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2986-*"):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2986.cpp").is_file():
        fails.append("AC6: tests/compiler/test_issue_2986.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2986 mutate Guard coverage — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
