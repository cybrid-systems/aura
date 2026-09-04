#!/usr/bin/env python3
"""Issue #3471: dual-fresh table domain is independent of C-bridge.

#3447 sampled C-bridge then washed table_ok when captured==cur_c_bridge.
Remount restamp of the shared bridge column made generation-behind
g_jit_fns go green.

Contract:
  AC1 captured==C-bridge does not force table_ok when table stamp differs
  AC2 single-eval table still moves; 2-arg tests (no stamp) stay green
  AC3 remount C-bridge restamp does not overwrite table stamp
  AC4 captured==0 + tracking stale on C-bridge (#2930); LEGACY_TRUST kept
  AC5 #3410 still fires; no new query key
  AC6 extend reemit suite; no test_issue_3471.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    br = (ROOT / "src" / "compiler" / "aura_jit_bridge.cpp").read_text()
    hh = (ROOT / "src" / "compiler" / "aura_jit_bridge.h").read_text()
    rt = (ROOT / "src" / "compiler" / "aura_jit_runtime.cpp").read_text()
    stub = (ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    mutate = (ROOT / "src" / "compiler" / "evaluator_primitives_mutate.cpp").read_text()

    fn = br.find("bool aura_is_jit_closure_fresh")
    body = br[fn : fn + 3200] if fn >= 0 else ""
    must("Issue #3471", "AC1 cite", body)
    must("captured_table_epoch", "AC1 table stamp arg", body)
    must("c_ok && table_ok", "AC1 AND kept", body)
    must_not("captured_bridge_epoch == cur_c_bridge", "AC1 wash gone", body)
    must("g_closure_table_epochs", "AC1 column", rt)
    must("stamp_closure_table_epoch_locked", "AC3 stamp helper", rt)
    # Remount C-bridge restamp must not assign table stamp.
    stamp = rt.find("static void stamp_closure_provenance_locked")
    stamp_win = rt[stamp : stamp + 1600] if stamp >= 0 else ""
    must_not("g_closure_table_epochs[cid]", "AC3 remount does not stamp table", stamp_win)
    must(
        "stamp_closure_table_epoch_locked(cid); // Issue #3471: remap retargeted catalog", "AC3 remap stamps table", rt
    )

    must("3471: C-bridge match does not wash table stamp miss", "AC5 test miss", test)
    must("3471: matching independent table stamp is fresh", "AC5 test hit", test)
    must("3471: captured==0 + C-bridge tracking still stale (#2930)", "AC4 zero stale", test)
    must("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "AC4 legacy trust kept", body)
    must("Issue #3410", "AC5 #3410 kept", rt)
    must("captured_table_epoch = 0", "AC1 header default", hh)
    must("captured_table_epoch", "AC1 stub", stub)
    must_not("schema-3471", "AC5 no query key", mutate)

    if (ROOT / "tests" / "compiler" / "test_issue_3471.cpp").is_file():
        fails.append("AC6: forbidden tests/compiler/test_issue_3471.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3471-*")):
            fails.append(f"AC6: docs/design/{f.name} present")

    if fails:
        print("FAIL #3471 jit_dual_fresh_table_stamp:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3471 jit_dual_fresh_table_stamp: independent table stamp, no C-bridge wash")
    return 0


if __name__ == "__main__":
    sys.exit(main())
