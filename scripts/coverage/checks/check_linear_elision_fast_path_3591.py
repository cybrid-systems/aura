#!/usr/bin/env python3
"""Issue #3591: every owned Move/Drop elision routes through #3006.

#3072 proved stolen-fiber Ready enqueue is dominated by
steal_safety_transaction Ok. This linter is the same machine-proof
pattern for linear Move/Drop elision:

  Scan lowering_linear_types_impl.cpp for elision (g_linear_move_elided_total
  fetch_add). Each function must call aura_linear_fast_path_ok() or
  aura_linear_fast_path_depth_or_densify_block() (the lowering-facing
  #3006 subset). The predicate must contain the epoch arm
  (invalidate_gen / linear_fast_path_rehydrate_gen_blocks_elision).
  Other hits fail CI with file:function.

  #2552 note_steal_or_densify_epoch_fence stays the occurrence fence
  primitive — do not invent a second epoch model.

Contract:
  AC1 HEAD elision sites whitelist-complete (or file:func fail).
  AC2 Synthetic unsynced elide must fail (teeth); synced ok() / block() pass.
  AC3 Production conjunct via aura_linear_fast_path_ok; Soft/Off skip;
      no new query key.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SCAN = ("src/compiler/lowering_linear_types_impl.cpp",)

_ELIDE = re.compile(r"\bg_linear_move_elided_total\s*\.fetch_add")
_PRED = re.compile(
    r"\baura_linear_fast_path_ok\s*\("
    r"|\baura_linear_fast_path_depth_or_densify_block\s*\("
)
_FN = re.compile(
    r"\b([A-Za-z_]\w*)\s*\((?:[^;{}]|::)*\)\s*(?:const\s*)?(?:noexcept(?:\s*\([^)]*\))?\s*)?\{",
)
_SKIP_NAMES = {"if", "while", "for", "switch", "catch", "else", "do"}


def _code_only(text: str) -> str:
    return "\n".join(ln.split("//", 1)[0] for ln in text.splitlines())


def _brace_body(s: str, open_idx: int) -> str:
    depth = 0
    i = open_idx
    n = len(s)
    while i < n:
        c = s[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return s[open_idx + 1 : i]
        i += 1
    return s[open_idx + 1 :]


def classify_elision_decisions(rel: str, text: str) -> list[str]:
    """Return file:function failures for elision without a #3006 predicate.

    Used on production sources and synthetic snippets so AC2 is
    executable: a naked g_linear_move_elided_total bump without
    aura_linear_fast_path_ok / depth_or_densify_block must fail.
    """
    fails: list[str] = []
    code = _code_only(text)
    seen: set[str] = set()
    for m in _FN.finditer(code):
        name = m.group(1)
        if name in _SKIP_NAMES:
            continue
        body = _brace_body(code, m.end() - 1)
        if not _ELIDE.search(body):
            continue
        if _PRED.search(body):
            continue
        key = f"{rel}:{name}"
        if key in seen:
            continue
        seen.add(key)
        fails.append(key)
    return fails


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # AC2 — teeth: unsynced elide fails; synced write is green.
    naked = (
        "std::optional<uint32_t> try_lower() {\n"
        "  g_linear_move_elided_total.fetch_add(1, std::memory_order_relaxed);\n"
        "  return inner;\n"
        "}\n"
    )
    if not classify_elision_decisions("src/compiler/lowering_linear_types_impl.cpp", naked):
        fails.append("AC2: classifier accepted unsynced Move elision (must fail CI)")
    synced_ok = (
        "std::optional<uint32_t> try_lower() {\n"
        "  if (aura_linear_fast_path_ok() != 0) {\n"
        "    g_linear_move_elided_total.fetch_add(1, std::memory_order_relaxed);\n"
        "    return inner;\n"
        "  }\n"
        "  return slot;\n"
        "}\n"
    )
    if classify_elision_decisions("src/compiler/lowering_linear_types_impl.cpp", synced_ok):
        fails.append("AC2: classifier rejected aura_linear_fast_path_ok-synced elision")
    synced_block = (
        "std::optional<uint32_t> try_lower() {\n"
        "  if (aura_linear_fast_path_depth_or_densify_block() == 0) {\n"
        "    g_linear_move_elided_total.fetch_add(1, std::memory_order_relaxed);\n"
        "    return inner;\n"
        "  }\n"
        "  return slot;\n"
        "}\n"
    )
    if classify_elision_decisions("src/compiler/lowering_linear_types_impl.cpp", synced_block):
        fails.append("AC2: classifier rejected depth_or_densify_block-synced elision")

    # AC1 — HEAD scan.
    for rel in SCAN:
        text = _read(rel)
        if not text:
            fails.append(f"AC1: missing {rel}")
            continue
        for msg in classify_elision_decisions(rel, text):
            fails.append(f"AC1 {msg}: Move elision without aura_linear_fast_path_ok / epoch block")

    low = _read("src/compiler/lowering_linear_types_impl.cpp")
    aud = _read("src/compiler/typed_mutation_audit.h")
    hooks = _read("src/compiler/typed_mutation_audit_hooks.cpp")
    gate = _read("src/compiler/ownership_escape_lowering_gate.h")
    tc = _read("src/compiler/evaluator_typecheck.cpp")
    test_esc = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    test_h = _read("tests/compiler/test_type_linear_commit_health.cpp")
    lint_self = _read("scripts/coverage/checks/check_linear_elision_fast_path_3591.py")

    must("aura_linear_fast_path_ok()", "AC1 lowering Production conjunct", low)
    must("aura_linear_fast_path_depth_or_densify_block()", "AC1 lowering epoch subset", low)
    must("aura_production_defaults_active_probe()", "AC3 Soft skip", low)
    must("Issue #3591", "AC1 lowering cite", low)
    must("kLinearElisionEpochFenceIssue = 3591", "AC1 stamp", gate)

    must("g_rehydrate_miss_invalidate_gen", "AC1 epoch arm in linear_fast_path_ok", aud)
    must("linear_fast_path_rehydrate_gen_blocks_elision", "AC1 epoch arm helper", aud)
    must("linear_fast_path_rehydrate_gen_blocks_elision()", "AC1 depth_or_densify epoch", hooks)
    must("note_steal_or_densify_epoch_fence", "AC1 #2552 fence reuse", tc)

    must("Issue #3591", "AC3591", lint_self)
    must("classify_elision_decisions", "AC3591", lint_self)
    must("ac3591_1_elision_routes_predicate", "AC3591", test_esc)
    must("ac3591_2_linter_rejects_unsynced_elide", "AC3591", test_esc)
    must("ac3591_3_epoch_arm_and_soft", "AC3591", test_h)

    qws = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    if "schema-3591" in qws:
        fails.append("AC3: schema-3591 present (forbidden new query key)")

    if (ROOT / "tests" / "compiler" / "test_issue_3591.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3591.cpp present (forbidden invent)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3591-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3591 linear elision #3006 epoch-fence machine proof — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
