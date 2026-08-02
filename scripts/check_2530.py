#!/usr/bin/env python3
"""Issue #2530: Audit ring 1024 + Isolation publish"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    src_core_capability_model_hh = _read("src/core/capability_model.hh")
    must("kAuditRing = 1024", "AC1", src_core_capability_model_hh)
    src_core_workspace_isolation_hh = _read("src/core/workspace_isolation.hh")
    must("kAuditRing = 1024", "AC1", src_core_workspace_isolation_hh)
    must("PublishedIsolationSlot", "AC2", src_core_workspace_isolation_hh)
    must("try_load_audit_seq", "AC2", src_core_workspace_isolation_hh)
    must("shared_mutex", "AC2", src_core_workspace_isolation_hh)
    CMakeLists_txt = _read("CMakeLists.txt")
    must("test_audit_ring_publish_2530", "AC6", CMakeLists_txt)
    build_py = _read("build.py")
    must("check_2530", "AC6", build_py)

    if fails:
        print("check_2530: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("check_2530: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
