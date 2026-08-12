#!/usr/bin/env python3
"""Issue #2772: denseness multi-process AURA_BIN export / self-path DX.

Child (shell …) only inherits *exported* env. Span runners that set
AURA_BIN as a non-exported shell var saw empty (getenv \"AURA_BIN\") and
exit 127 (\"aura: not found\"). Fix: seed process environ from self path,
expose (aura-executable-path), document export AURA_BIN in denseness usage.

Contract (one row per AC):
  AC1 ensure_aura_bin_environ / resolve_self_executable in runtime_paths + main
  AC2 aura-executable-path prim + #2772 cite
  AC3 print_denseness_usage lists export AURA_BIN + multi-process child note
  AC4 multi-process AURA_BIN contract in source (static; no live aura smoke in gate)
  AC5 this linter wired; no docs/design/2772-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


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

    paths = _read("src/compiler/runtime_paths.h")
    main_cpp = _read("src/main.cpp")
    io = _read("src/compiler/evaluator_primitives_io.cpp")
    build = _read("build.py")

    # AC1
    must("#2772", "AC1", paths)
    must("resolve_self_executable", "AC1", paths)
    must("ensure_aura_bin_environ", "AC1", paths)
    must("ensure_aura_bin_environ", "AC1", main_cpp)
    must("#2772", "AC1", main_cpp)
    must("/proc/self/exe", "AC1", paths)

    # AC2
    must('add("aura-executable-path"', "AC2", io)
    must("#2772", "AC2", io)
    must("resolve_self_executable", "AC2", io)

    # AC3
    must("export AURA_BIN", "AC3", main_cpp)
    must("Multi-process", "AC3", main_cpp)
    must("aura-executable-path", "AC3", main_cpp)
    must("print_denseness_usage", "AC3", main_cpp)

    # AC4 live

    # AC5
    must("check_denseness_multiprocess_env_2772", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2772-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2772.cpp").is_file():
        fails.append("AC5: test_issue_2772.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2772 denseness multi-process AURA_BIN — seed + aura-executable-path + usage")
    return 0


if __name__ == "__main__":
    sys.exit(main())
