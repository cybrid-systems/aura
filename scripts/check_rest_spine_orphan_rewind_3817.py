#!/usr/bin/env python3
"""Issue #3817: dotted-rest MacroIntroduced spine rewind on clone deny.

Rest add_*/stamp before clone_macro_body sits outside ExpandCheckpointGuard
size0. Production must checkpoint FlatAST size before rest stamp and
truncate_to on NULL clone. Soft/Off keeps historical half-write.

Contract (one row per AC):
  AC1  eval_flat cites #3817 + rest_spine_ckpt + truncate_to on NULL
  AC2  reexpand_call + expand_inner + macro_expand_all same face
  AC3  Tests extend test_rest_param_hygiene_eval_flat; no invent / docs/design
  AC4  Soft/Off gate via is_sandbox_active / production_surface

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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

    eef = _read("src/compiler/evaluator_eval_flat.cpp")
    mx = _read("src/compiler/macro_expansion.cpp")
    test = _read("tests/compiler/test_rest_param_hygiene_eval_flat.cpp")
    build = _read("build.py")

    must("Issue #3817", "AC1", eef)
    must("rest_spine_ckpt", "AC1", eef)
    must("truncate_to(rest_spine_ckpt)", "AC1", eef)
    must("Issue #3817: production rewind pre-clone", "AC1", eef)
    must("is_sandbox_active()", "AC1", eef)

    must("Issue #3817", "AC2", mx)
    must("truncate_to(rest_spine_ckpt)", "AC2", mx)
    must("rest_spine_pending", "AC2", eef)
    # reexpand_call cite
    must("Issue #3817: checkpoint before rest add_*/stamp", "AC2", eef)

    must("3817", "AC3", test)
    must("3817 soak", "AC3", test)
    must("flat size restored", "AC3", test)
    must("hygiene-gensym-ceiling", "AC3", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3817.cpp").is_file():
        fails.append("AC3: test_issue_3817.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3817-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden per #1655)")
    must("check_rest_spine_orphan_rewind_3817", "AC3", build)

    must("is_sandbox_active()", "AC4", eef[eef.find("Issue #3817: production rewind") :][:600])
    must("production_surface", "AC4", mx)
    must("3817 AC3", "AC4", test)

    if fails:
        print("FAIL: Issue #3817 rest-spine orphan rewind")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3817 dotted-rest MacroIntroduced spine rewind on clone deny")
    return 0


if __name__ == "__main__":
    sys.exit(main())
