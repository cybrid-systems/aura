#!/usr/bin/env python3
"""Issue #3172: sink fine-grained compile: dirty primitives.

Public compile: add() surface is snapshot + relower-strategy (≤5).
mark/clear/dirty?/counts/per-defuse/hw-bitvec/subtree-bump stay as
sink_compile_prim bodies (Guard + IR hooks) but are not registered.

  AC1 Public compile: add() names ≤ 5 and only snapshot + relower-strategy
  AC2 sink_compile_prim holds the 21 sunk names; no add("compile:mark-…
  AC3 Soft / existing Guard helpers unchanged on sunk bodies
  AC4 No new public query key; SlimSurface shrinks
  AC5 Extend compile-primitive-guard + capability + steal suites
  AC6 This linter + build.py; no test_issue_3172.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]

ADD_RE = re.compile(r'add\(\s*"([^"]+)"')
SUNK = (
    "compile:mark-dirty-upward-fast",
    "compile:block-dirty-count",
    "compile:func-block-dirty-count",
    "compile:block-dirty?",
    "compile:mark-block-dirty!",
    "compile:clear-block-dirty!",
    "compile:is-instruction-dirty?",
    "compile:mark-instruction-dirty!",
    "compile:clear-instruction-dirty!",
    "compile:verify-dirty?",
    "compile:macro-dirty?",
    "compile:clear-macro-dirty!",
    "compile:mark-narrowing-dirty!",
    "compile:narrowing-dirty?",
    "compile:per-defuse-index-add",
    "compile:per-defuse-index-callers",
    "compile:subtree-bump",
    "compile:hw-bitvec-register",
    "compile:hw-bitvec-width",
    "compile:hw-bitvec-signed?",
    "compile:hw-bitvec-compatible?",
)
KEEP = ("compile:snapshot", "compile:relower-strategy")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    compile_cpp = _read("src/compiler/evaluator_primitives_compile.cpp")
    ctor = _read("src/compiler/evaluator_ctor.cpp")
    guard = _read("tests/compiler/test_compile_primitive_guard.cpp")
    cap = _read("tests/compiler/test_capability_gating.cpp")
    build = _read("build.py")
    q = read_query_prims()

    public = [n for n in ADD_RE.findall(compile_cpp) if n.startswith("compile:")]
    if len(public) > 5:
        fails.append(f"AC1: public compile: add() count {len(public)} > 5: {public}")
    for k in KEEP:
        if k not in public:
            fails.append(f"AC1: missing public {k}")
    extra = sorted(set(public) - set(KEEP))
    if extra:
        fails.append(f"AC1: unexpected public compile: {extra}")

    must("sink_compile_prim", "AC2 helper", compile_cpp)
    must("Issue #3172", "AC2 cite", compile_cpp)
    for name in SUNK:
        if f'sink_compile_prim("{name}"' not in compile_cpp:
            fails.append(f"AC2: sunk body missing {name}")
        if f'add("{name}"' in compile_cpp:
            fails.append(f"AC2: public add() still registers {name}")

    must("run_compile_dirty_under_guard", "AC3 Guard helper", compile_cpp)
    must("sunk from the public PrimRegistrar", "AC3 ctor", ctor)

    if "query:compile-dirty-op" in q or "query:compile-surface" in q:
        fails.append("AC4: new top-level query key (forbidden)")
    must("3172: snapshot public", "AC5 test", guard)
    must("sunk compile: dirty names", "AC5 cap", cap)
    must("check_compile_surface_3172", "AC6 build", build)
    must("cmd_compile_surface_3172", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3172.cpp").is_file():
        fails.append("AC6: test_issue_3172.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3172-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3172 compile: surface reduction — public={public} sunk={len(SUNK)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
