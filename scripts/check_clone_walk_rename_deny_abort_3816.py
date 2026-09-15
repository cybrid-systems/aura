#!/usr/bin/env python3
"""Issue #3816: clone-walk rename_binding deny aborts before add_*/marker.

Mirror #3506 after param/rename phase under production_surface: try_restore
+ return NULL_NODE before add_lambda / add_let / set_marker. Soft/Off
(historical half-write) unchanged. hyg_ctr still unadvanced on deny (#2811).

Contract (one row per AC):
  AC1  Post-param production deny abort cites #3816 + try_restore
  AC2  Let/Define rename deny aborts before add_*; hyg_ctr before ++
  AC3  Tests extend test_clone_walk_gensym_ceiling; no invent / docs/design
  AC4  Soft/Off continue face retained (production_surface gate)

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

    me = _read("src/compiler/macro_expansion.cpp")
    test = _read("tests/compiler/test_clone_walk_gensym_ceiling.cpp")
    build = _read("build.py")

    must("Issue #3816", "AC1", me)
    params = me.find("std::vector<aura::ast::SymId> param_syms;")
    win = me[params : params + 2200] if params >= 0 else ""
    must("Issue #3816", "AC1", win)
    must("inner_expand_production_limit_deny()", "AC1", win)
    must("expand_ckpt.try_restore()", "AC1", win)
    must("return aura::ast::NULL_NODE", "AC1", win)
    must("param_syms", "AC1", win)

    # AC2: Let / Define guards + hyg_ctr order in rename_binding
    must("abort before add_let", "AC2", me)
    must("abort before add_define", "AC2", me)
    rb = me.find("auto rename_binding =")
    rb_win = me[rb : rb + 2800] if rb >= 0 else ""
    ceil = rb_win.find("gensym_cap")
    deny_ret = rb_win.find("return aura::ast::NULL_NODE", ceil if ceil >= 0 else 0)
    hyg_inc = rb_win.find("hyg_ctr++", ceil if ceil >= 0 else 0)
    if not (deny_ret >= 0 and hyg_inc >= 0 and deny_ret < hyg_inc):
        fails.append("AC2: rename_binding deny must return before hyg_ctr++")

    # AC3: extend existing suite; no invent / docs/design
    must("3816", "AC3", test)
    must("3816 soak", "AC3", test)
    must("flat size restored", "AC3", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3816.cpp").is_file():
        fails.append("AC3: test_issue_3816.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3816-*")):
            fails.append(f"AC3: docs/design/{f.name} present (forbidden per #1655)")
    must("check_clone_walk_rename_deny_abort_3816", "AC3", build)

    # AC4: Soft/Off gate via production_surface (Off → false)
    must("production_surface && inner_expand_production_limit_deny()", "AC4", win)
    must("3816 AC3", "AC4", test)

    if fails:
        print("FAIL: Issue #3816 clone-walk rename deny abort")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3816 clone-walk rename_binding deny aborts before add_*/marker")
    return 0


if __name__ == "__main__":
    sys.exit(main())
