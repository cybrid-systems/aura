#!/usr/bin/env python3
"""Issues #4272 + #4273: Soft set-code CASE parity with oneshot + pick-best.

#4272 residual after #4264: set-code + eval-current under-counted / over-
counted vs oneshot.
  Under-count: #3235 heap-mutate auto-Guard fail-closed under the
  eval-current pin (vector-set!/hash-set!/set-car! → soft <error> after
  CASE0=). Fix: maybe_auto_guard_heap_mutate allows under
  eval_current_holds_shared_pin.
  Over-count (WAVE16 course-schedule): parse_to_flat recovered past
  leftover top-level ')' and scored full CASE while oneshot (#3917)
  hard-failed empty. Fix: parse hard_fail_extra_close (#4272/#3917 parity).

#4273: pick-best soft_bad_value / unbound lst|i / soft_mismatch:-999999 /
cannot call: >. Faces: (a) soft Error as status=ok value=<error>;
(b) workspace (define > …) left top_ shadows poisoning Path B pick-best.
Fix: emit_exec_result → status=error on soft Error; set-code/eval-current
drop prim shadows from top_.

ACs:
  AC1 #4272 door — test_workspace_lock_reentrancy AC7 present + invoked
  AC2 #4272 source — maybe_auto_guard cites #4272 + pin short-circuit
  AC3 #4273 door — test_primcall_narg AC23 present + invoked
  AC4 #4273 source — eval primitives + serve emit_exec_result cite #4273
  AC5 build.py wiring + allowlist; no docs/design/4272-* or 4273-*
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(cond: bool, msg: str) -> None:
    if not cond:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"OK: {msg}")


def main() -> None:
    ws = (ROOT / "tests/core/test_workspace_lock_reentrancy.cpp").read_text(encoding="utf-8")
    check("AC7 (#4272)" in ws or "#4272" in ws, "AC1: workspace lock test cites #4272")
    check("vector-set! under set-code + eval-current" in ws, "AC1: AC7 vector-set! door present")

    mb = (ROOT / "src/compiler/evaluator_mutation_boundary.cpp").read_text(encoding="utf-8")
    check("Issue #4272" in mb, "AC2: maybe_auto_guard cites #4272")
    check(
        "eval_current_holds_shared_pin()" in mb and "maybe_auto_guard_heap_mutate" in mb,
        "AC2: heap-mutate short-circuit under eval-current pin",
    )

    # Over-count face (WAVE16): parse_to_flat hard-fails extra top-level )
    parser = (ROOT / "src/parser/parser_impl.cpp").read_text(encoding="utf-8")
    check("Issue #4272" in parser, "AC2b: parser cites #4272")
    check("hard_fail_extra_close" in parser, "AC2b: hard_fail_extra_close present")
    check("unexpected ')'" in parser, "AC2b: unexpected ) message")

    ws = (ROOT / "tests/core/test_workspace_lock_reentrancy.cpp").read_text(encoding="utf-8")
    check("AC8 (#4272)" in ws or "AC8 (#4272 over-count)" in ws, "AC2b: AC8 over-count door present")
    check("set-code rejects" in ws or "extra close-paren" in ws, "AC2b: AC8 set-code reject door")

    tc = (ROOT / "tests/compiler/test_primcall_narg.cpp").read_text(encoding="utf-8")
    check("ac23_pick_best_prim_shadow_4273" in tc, "AC3: AC23 door present")
    check("ac23_pick_best_prim_shadow_4273();" in tc, "AC3: AC23 invoked from run_test")
    check("#4273" in tc, "AC3: test cites #4273")

    ev = (ROOT / "src/compiler/evaluator_primitives_eval.cpp").read_text(encoding="utf-8")
    check("Issue #4273" in ev, "AC4: eval primitives cite #4273")
    check("unbind_local" in ev and "lookup_primitive" in ev, "AC4: prim shadow drop")

    serve = (ROOT / "src/serve/serve_async.cpp").read_text(encoding="utf-8")
    check("emit_exec_result" in serve, "AC4: serve emit_exec_result")
    check("soft_error_message" in serve or "Issue #4273" in serve, "AC4: serve cites soft error path")

    eix = (ROOT / "src/compiler/evaluator.ixx").read_text(encoding="utf-8")
    check("soft_error_message" in eix, "AC4: Evaluator::soft_error_message")

    build = (ROOT / "build.py").read_text(encoding="utf-8")
    check(
        "check_setcode_pick_parity_4272_4273" in build,
        "AC5: build.py wires check_setcode_pick_parity_4272_4273.py",
    )
    allow = (ROOT / "scripts/coverage/root_check_allowlist.txt").read_text(encoding="utf-8")
    check(
        "check_setcode_pick_parity_4272_4273.py" in allow,
        "AC5: root_check_allowlist entry",
    )
    design = list((ROOT / "docs/design").glob("4272-*")) + list((ROOT / "docs/design").glob("4273-*"))
    check(not design, "AC5: no docs/design/4272-* or 4273-*")
    print("OK: Issues #4272+#4273 set-code/pick parity — all AC rows satisfied")


if __name__ == "__main__":
    main()
