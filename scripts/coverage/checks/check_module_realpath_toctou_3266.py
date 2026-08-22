#!/usr/bin/env python3
"""Issue #3266: resolve_module_path realpath-then-lstat + lock granularity.

try_load must realpath first then lstat the canonical path (no
stat-then-realpath TOCTOU). is_plausible_module_path runs before
compact_env_frames_lock_ so refuse paths take no lock (zero extra).
loading_stack_ lock-released I/O window is documented.

Contract:
  AC1  realpath first, then lstat canonical S_ISREG
  AC2  validate before compact_env_frames_lock_
  AC3  loading_stack_ I/O window comment
  AC4  empty refuse still void; missing path realpath-null skip
  AC5  extend test_module_path_refuse; linter after #3265; no invent

Exit 0 = all rows satisfied.

Follow-up #3267: publish_live_env_linear_to_bridge env_frames_ lock + combined bridge state.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    loader = _read("src/compiler/evaluator_module_loader.cpp")
    test = _read("tests/compiler/test_module_path_refuse.cpp")
    build = _read("build.py")
    l3265 = _read("scripts/coverage/checks/check_jit_fn_marker_atomic_3265.py")

    pos = loader.find("auto try_load = [](const std::string& full)")
    win = loader[pos : pos + 900] if pos >= 0 else ""
    must("Issue #3266", "AC1 cite", win)
    must("::realpath(candidate.c_str(), nullptr)", "AC1 realpath", win)
    must("::lstat(out.c_str(), &lst)", "AC1 lstat", win)
    must("S_ISREG(lst.st_mode)", "AC1 regular", win)
    if "::stat(candidate.c_str(), &st)" in win:
        fails.append("AC1: stat-then-realpath still present")
    must("ac3266_1_realpath_then_lstat", "AC1 test", test)

    lpos = loader.find("Evaluator::load_module_file")
    lwin = loader[lpos : lpos + 1400] if lpos >= 0 else ""
    val = lwin.find("is_plausible_module_path(path)")
    lock = lwin.find("std::lock_guard interlock(compact_env_frames_lock_)")
    if val < 0 or lock < 0 or val > lock:
        fails.append("AC2: validate must precede compact_env_frames_lock_")
    must("Issue #3266", "AC2 cite", lwin)
    must("zero extra", "AC2 refuse no lock", lwin)
    must("ac3266_2_validate_before_lock", "AC2 test", test)

    must("lock released for file I/O on purpose", "AC3 comment", loader)
    must("module_cache_ is populated before erase", "AC3 cache-before-erase", loader)
    must("ac3266_3_loading_stack_window_comment", "AC3 test", test)

    must("if (!real)", "AC4 missing skip", win)
    must("ac3266_4_quiet_refuse_and_load", "AC4 test", test)

    must("ac3266_5_source_and_linter", "AC5 test", test)
    must("check_module_realpath_toctou_3266", "AC5 build.py", build)
    prev = build.find("check_jit_fn_marker_atomic_3265")
    ours = build.find("check_module_realpath_toctou_3266")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3265")
    must("3266", "AC5 extend 3265 linter", l3265)
    if (ROOT / "tests" / "issues" / "test_issue_3266.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3266.cpp per #81967")
    if (ROOT / "tests" / "compiler" / "test_issue_3266.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3266.cpp per #81967")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3266-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")
    q = _read("src/compiler/evaluator_primitives_query_tail.cpp")
    if "schema-3266" in q or "schema-3266" in test:
        fails.append("AC5: new schema-3266 query key (SlimSurface)")

    if fails:
        print("FAIL #3266 module_realpath_toctou:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3266 module_realpath_toctou: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
