#!/usr/bin/env python3
"""Issue #3341 linter — per-fiber last_limit_reason + steal-abort Agent string.

Residuals:
  1. note_hygiene_last_limit_reason stored a process-global atomic only.
     Concurrent fibers last-writer-wins; Agent cannot correlate a reject
     to a specific expand / mutate attempt.
  2. Some steal-abort paths bumped g_macro_clone_steal_abort_total without
     a durable last_mutate_error_ string.
  3. aura_macro_provenance_repin_on_steal nullptr residual on production
     clone (per-Evaluator dual-write stays file-level).

Fix (no second hygiene model, no new query:* name):
  A. FiberHygieneStats.last_limit_reason; note_* also stamps current fiber.
  B. steal-abort site calls aura_evaluator_note_steal_abort_mid_expand()
     → last_mutate_error_ = "steal-abort-mid-expand".
  C. production clone resolves Evaluator* before repin (never nullptr
     literal). Soft/Off: extra store only on reject (no new TLS / walks).

Gate rows:
  G1  FiberHygieneStats has last_limit_reason.
  G2  note_hygiene_last_limit_reason stamps per-fiber via
      stamp_fiber_last_limit_reason / note_hygiene_last_limit_reason_for_fiber.
  G3  steal-abort site (g_macro_clone_steal_abort_total.fetch_add) calls
      aura_evaluator_note_steal_abort_mid_expand.
  G4  Evaluator notes "steal-abort-mid-expand" on last_mutate_error_.
  G5  production clone does not pass nullptr to repin.
  G6  gensym-ceiling deny routes through note_* (per-fiber stamp).
  G7  tests: test_macro_fiber_hygiene + steal_abort_visibility +
      test_hygiene_mutate_closed_loop.
  G8  build.py wires this linter AFTER #3340.
  G9  no docs/design/3341-*, no test_issue_3341.cpp.
  G10 no new query:* name.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]

failures: list[str] = []


def must(ok: bool, label: str) -> None:
    if ok:
        print(f"  OK: {label}")
    else:
        failures.append(label)
        print(f"  FAIL: {label}")


def read(rel: str) -> str:
    p = ROOT / rel
    try:
        return p.read_text()
    except FileNotFoundError:
        return ""


def main() -> int:
    print("=== #3341 per-fiber last_limit_reason + steal-abort Agent string ===")
    ixx = read("src/compiler/macro_expansion.ixx")
    me = read("src/compiler/macro_expansion.cpp")
    ev = read("src/compiler/evaluator.ixx")
    fiber = read("src/compiler/evaluator_fiber_mutation.cpp")
    hdr = read("src/compiler/aura_jit_bridge.h")
    fiber_test = read("tests/compiler/test_macro_fiber_hygiene.cpp")
    steal_test = read("tests/compiler/test_concurrent_clone_steal_abort_visibility.cpp")
    loop = read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    build = read("build.py")
    q_eval = read("src/compiler/evaluator_primitives_obs_eval.cpp")
    q_mid = read("src/compiler/evaluator_primitives_query_obs_mid.cpp")

    must("last_limit_reason = 0" in ixx, "G1: FiberHygieneStats.last_limit_reason")
    must(
        "stamp_fiber_last_limit_reason" in me and "note_hygiene_last_limit_reason_for_fiber" in ixx,
        "G2: note_* stamps per-fiber last_limit_reason",
    )
    pos = me.find("g_macro_clone_steal_abort_total.fetch_add")
    scope = me[pos : pos + 1600] if pos >= 0 else ""
    must(
        pos >= 0 and "aura_evaluator_note_steal_abort_mid_expand" in scope,
        "G3: steal-abort site stamps last_mutate_error_ helper",
    )
    must(
        "steal-abort-mid-expand" in ev
        and "aura_evaluator_note_steal_abort_mid_expand" in fiber
        and "aura_evaluator_note_steal_abort_mid_expand" in hdr,
        "G4: Evaluator + C ABI note steal-abort-mid-expand",
    )
    must(
        "aura_macro_provenance_repin_on_steal(nullptr" not in me and "aura_evaluator_resolve_current_for_macro" in me,
        "G5: production clone does not pass nullptr to repin",
    )
    must(
        "note_hygiene_last_limit_reason(kHygieneLimitReasonGensymCeiling)" in me,
        "G6: gensym-ceiling deny routes through note_*",
    )
    must("ac3341_per_fiber_last_limit_reason" in fiber_test, "G7a: fiber hygiene AC")
    must("steal-abort-mid-expand Agent string" in steal_test, "G7b: steal-abort suite")
    must("ac3341_per_fiber_reason_and_steal_abort_string" in loop, "G7c: closed-loop AC")
    must(
        "check_fiber_hygiene_last_limit_reason_3341.py" in build,
        "G8: build.py wires linter",
    )
    i3340 = build.find("check_cross_flat_provenance_homology_3340.py")
    i3341 = build.find("check_fiber_hygiene_last_limit_reason_3341.py")
    must(i3340 >= 0 and i3341 > i3340, "G8b: linter wired AFTER #3340")
    must(
        not any(p.name.startswith("3341-") for p in (ROOT / "docs/design").glob("3341-*"))
        if (ROOT / "docs/design").exists()
        else True,
        "G9a: no docs/design/3341-* per #1655",
    )
    must(
        not (ROOT / "tests/issues" / "test_issue_3341.cpp").exists(),
        "G9b: no tests/issue*/test_issue_3341.cpp per #81967",
    )
    must(
        "query:fiber-last-limit-reason" not in q_eval
        and "query:steal-abort-mid-expand" not in q_eval
        and "query:fiber-last-limit-reason" not in q_mid
        and "query:steal-abort-mid-expand" not in q_mid,
        "G10: no new query:* name",
    )

    if failures:
        print(f"\n#3341 linter FAILED: {len(failures)} gate(s)")
        return 1
    print("\nOK #3341 fiber_hygiene_last_limit_reason: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
