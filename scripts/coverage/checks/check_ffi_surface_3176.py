#!/usr/bin/env python3
"""Issue #3176: demote C FFI c-* prims off core boot.

c-load/c-func/c-alloc/c-free/c-opaque/c-struct-* install on
(require "std/ffi"). Sandbox without kEffectFfi refuses the module.

  AC1 Evaluator ctor defers c-* via defer_std_host_prim
  AC2 std/ffi.aura value aliases; INDEX lists std/ffi
  AC3 ensure_std_host_prims ffi + kEffectFfi
  AC4 No new public query key
  AC5 Extend require_effect_live_mid (sandbox deny + unbound before require)
  AC6 This linter + build.py; no test_issue_3176.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]

DEFERRED = (
    "c-load",
    "c-func",
    "c-alloc",
    "c-free",
    "c-opaque",
    "c-opaque?",
    "c-opaque->int",
    "c-struct-size",
    "c-struct-set!",
    "c-struct-ref",
)


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    ctor = _read("src/compiler/evaluator_ctor.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    ffi_impl = _read("src/compiler/ffi_primitives_impl.cpp")
    ffi = _read("lib/std/ffi.aura")
    idx = _read("lib/std/INDEX.aura")
    t = _read("tests/compiler/test_require_effect_live_mid.cpp")
    build = _read("build.py")
    q = read_query_prims()

    must("Issue #3176", "AC1 cite", ctor)
    must('name.starts_with("c-")', "AC1 ctor defer", ctor)
    must("defer_std_host_prim", "AC1 ctor defer call", ctor)
    for name in DEFERRED:
        needle = f'add("{name}"'
        if needle not in ffi_impl:
            fails.append(f"AC1: ffi body missing {name}")

    must("(define c-func c-func)", "AC2 alias", ffi)
    must("(define c-load c-load)", "AC2 alias load", ffi)
    must("std/ffi", "AC2 INDEX", idx)
    must("#3176", "AC2 INDEX cite", idx)

    must('is_mod("ffi")', "AC3 loader", loader)
    must("kEffectFfi", "AC3 ffi effect", loader)
    must("std/ffi", "AC3 ixx", ixx)

    if "query:std-ffi" in q or "query:ffi-surface" in q:
        fails.append("AC4: new top-level query key (forbidden)")

    must("3176: c-opaque? unbound before std/ffi", "AC5 test", t)
    must('ensure_std_host_prims("std/ffi")', "AC5 install", t)
    must("sandbox without ffi grant refuses std/ffi", "AC5 deny", t)

    must("check_ffi_surface_3176", "AC6 build", build)
    must("cmd_ffi_surface_3176", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3176.cpp").is_file():
        fails.append("AC6: test_issue_3176.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3176-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3176 C FFI demotion — deferred={len(DEFERRED)} via std/ffi + kEffectFfi")
    return 0


if __name__ == "__main__":
    sys.exit(main())
