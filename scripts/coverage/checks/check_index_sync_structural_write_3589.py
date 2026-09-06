#!/usr/bin/env python3
"""Issue #3589: every structural AST write path syncs tag_arity / DefUse index.

#3072 proved stolen-fiber Ready enqueue is dominated by steal_safety_transaction
Ok. This linter is the same machine-proof pattern for EDSL index sync:

  Scan mutation.ixx / mutators.ixx / ast_mutation_pipeline.ixx for
  structural write functions (set_child / insert_child / remove_child /
  try_move_child / children_[]= / parent_[]=). Each must call
  mark_dirty_upward (or _fast/_until/_masked/_with_index_update) or
  explicit index sync (tag_arity_index_* / invalidate_tag_arity_index /
  DefUseIndex invalidate). Other hits fail CI with file:function.

Contract:
  AC1 HEAD of the three modules is whitelist-complete (or file:func fail).
  AC2 Synthetic unsynced set_child must fail (teeth).
  AC3 Pure static; no runtime change; no new query key.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SCAN = (
    "src/core/mutation.ixx",
    "src/core/mutators.ixx",
    "src/core/ast_mutation_pipeline.ixx",
)

_WRITE = re.compile(
    r"\b(?:set_child|insert_child|remove_child|try_move_child|replace_child|move_child)\s*\("
    r"|\bchildren_\s*\[[^\]]+\]\s*="
    r"|\bparent_\s*\[[^\]]+\]\s*="
)
_SYNC = re.compile(
    r"\bmark_dirty_upward(?:_fast|_until|_masked|_with_index_update)?\s*\("
    r"|\btag_arity_index_(?:insert_node|remove_node|rebuild_full)\s*\("
    r"|\binvalidate_tag_arity_index\s*\("
    r"|\b(?:invalidate_defuse|defuse_index_invalidate|DefUseIndex)"
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


def _qualify(prefix: str, name: str) -> str:
    structs = list(re.finditer(r"\b(?:export\s+)?struct\s+(\w+)", prefix))
    for m in reversed(structs):
        rest = prefix[m.end() :]
        if rest.count("{") - rest.count("}") > 0:
            return f"{m.group(1)}::{name}"
    return name


def classify_structural_writes(rel: str, text: str) -> list[str]:
    """Return file:function failures for unsynced structural writes.

    Used on production sources and synthetic snippets so AC2 is
    executable: a naked set_child without mark_dirty_upward must fail.
    """
    fails: list[str] = []
    code = _code_only(text)
    seen: set[str] = set()
    for m in _FN.finditer(code):
        name = m.group(1)
        if name in _SKIP_NAMES:
            continue
        body = _brace_body(code, m.end() - 1)
        if not _WRITE.search(body):
            continue
        if _SYNC.search(body):
            continue
        qname = _qualify(code[: m.start()], name)
        key = f"{rel}:{qname}"
        if key in seen:
            continue
        seen.add(key)
        fails.append(key)
    return fails


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    # AC2 — teeth: unsynced write fails; synced write is green.
    naked = "struct Evil {\n  void apply(FlatAST& flat, NodeId t) {\n    flat.set_child(t, 0, NULL_NODE);\n  }\n};\n"
    naked_fails = classify_structural_writes("src/core/mutators.ixx", naked)
    if not naked_fails:
        fails.append("AC2: classifier accepted unsynced set_child (must fail CI)")
    synced = (
        "struct Ok {\n"
        "  void apply(FlatAST& flat, NodeId t) {\n"
        "    flat.set_child(t, 0, NULL_NODE);\n"
        "    flat.mark_dirty_upward(t);\n"
        "  }\n"
        "};\n"
    )
    if classify_structural_writes("src/core/mutators.ixx", synced):
        fails.append("AC2: classifier rejected mark_dirty_upward-synced write")

    # AC1 — HEAD scan of the three named modules.
    for rel in SCAN:
        text = _read(rel)
        if not text:
            fails.append(f"AC1: missing {rel}")
            continue
        for msg in classify_structural_writes(rel, text):
            fails.append(f"AC1 {msg}: structural write without mark_dirty_upward / index sync")

    mut = _read("src/core/mutators.ixx")
    must("mark_dirty_upward(target)", "AC1 mutators Replace/Insert/Remove", mut)
    astc = _read("src/core/ast.ixx")
    must("tag_arity_index. mark_dirty_upward + structural mutate", "AC1 ast.ixx contract", astc)

    test = _read("tests/core/test_tag_arity_index_lock.cpp")
    lint_self = _read("scripts/coverage/checks/check_index_sync_structural_write_3589.py")
    must("Issue #3589", "AC3589", lint_self)
    must("classify_structural_writes", "AC3589", lint_self)
    must("ac3589_1_head_scan_green", "AC3589", test)
    must("ac3589_2_linter_rejects_unsynced_write", "AC3589", test)
    must("ac3589_3_static_no_runtime", "AC3589", test)

    if (ROOT / "tests" / "core" / "test_issue_3589.cpp").is_file():
        fails.append("AC3: tests/core/test_issue_3589.cpp present (forbidden invent)")
    if (ROOT / "tests" / "compiler" / "test_issue_3589.cpp").is_file():
        fails.append("AC3: tests/compiler/test_issue_3589.cpp present (forbidden invent)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3589-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #3589 index-sync structural-write machine proof — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
