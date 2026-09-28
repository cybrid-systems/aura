#!/usr/bin/env python3
"""Issue #4145 source-cite gate: Soft value-only intermediate auto-wire must
be unreachable under production required (dual-track allocate residual).

`note_intermediate_create_auto_wire_` was the last value-only seam of the
#3156/#3306 dual-track: `note_intermediate_create_with_cover_` routes
required + both-null through the fail-closed inventory arm, and the pre-move
gate OR-clause blocks when `value_only_total > 0` — but the auto-wire helper
itself stayed callable under required. Any new allocate/create site that hit
it under required (a gate bypass, or a Soft/required latch race) registered
a value-only densify root (observability only, NOT safe cover per #3017):
soak invariant `value_only_total == 0` broken and sticky-off until
recovery; missed-remap window reopened. #4145 arms the helper itself: under
`general_object_pin_required_active()` it fail-closes into the same #3156
uncovered inventory arm (push_back + uncovered counter, pre-move gate
blocks + sticky-off, no relocate) and never touches
`register_external_root_for_densify` / the value-only bump. Soft / Off
keeps the zero-cost auto-wire contract (single relaxed load + branch).
No second model — the existing #3017/#3156/#3306 counters and inventory
stay the SSOT.

ACs:
  AC1  the required-face guard lives inside `note_intermediate_create_
       auto_wire_`: cites #4145, checks `general_object_pin_required_
       active()`, fail-closes into `g_intermediate_create_uncovered_
       under_required_total` (+ inventory push), and the guard is ordered
       BEFORE `register_external_root_for_densify(p)` and the
       `g_intermediate_create_value_only_total` bump (comment-stripped).
  AC2  exactly one live `note_intermediate_create_auto_wire_(p);` call
       site survives in comment-stripped arena.ixx — the Soft/Off
       fallback — and `note_intermediate_create_with_cover_`'s required
       arm precedes that fallback in the function body.
  AC3  soak SSOT unchanged: the #3306 pre-move OR-clause
       (`intermediate_create_value_only_total_v_read() > 0`) stays, the
       `g_intermediate_create_value_only_total{0}` export stays, the Soft
       value-only bump stays behind the guard, and no second
       value-only/auto-wire registry was invented.
  AC4  runtime doors: test_moving_densify_fail_closed.cpp cites #4145 with
       dispatched ac4145_1..4 calls; no tests/core/test_issue_4145.cpp;
       no docs/design/4145-*; build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARENA = ROOT / "src" / "core" / "arena.ixx"
TST = ROOT / "tests" / "core" / "test_moving_densify_fail_closed.cpp"

AUTO_WIRE_DEF = "void note_intermediate_create_auto_wire_(void* p) noexcept"
AUTO_WIRE_CALL = "note_intermediate_create_auto_wire_(p);"


def _strip_comments(text: str) -> str:
    out: list[str] = []
    for line in text.splitlines():
        cut = line.find("//")
        out.append(line[:cut] if cut >= 0 else line)
    return "\n".join(out)


def main() -> int:
    arena_raw = ARENA.read_text() if ARENA.exists() else ""
    arena = _strip_comments(arena_raw)
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        if not good:
            ok = False
        print(f"  [{'PASS' if good else 'FAIL'}] {name}: {msg}")

    # AC1 — required-face guard inside note_intermediate_create_auto_wire_,
    # ordered before any value-only registration.
    fn = arena.find(AUTO_WIRE_DEF)
    report(
        "AC1.helper",
        fn != -1,
        "note_intermediate_create_auto_wire_ definition present in arena.ixx",
    )
    if fn != -1:
        win = arena[fn : fn + 2200]
        guard = win.find("general_object_pin_required_active()")
        unc = win.find("g_intermediate_create_uncovered_under_required_total.fetch_add")
        root = win.find("register_external_root_for_densify(p)")
        vo = win.find("g_intermediate_create_value_only_total.fetch_add")
        push = win.find("intermediate_creates_.push_back(p)")
        ret = win.find("return;", guard if guard != -1 else 0)
        cite = arena_raw.find("Issue #4145", fn)
        report(
            "AC1.guard",
            guard != -1 and unc != -1 and push != -1,
            "guard checks required_active, fail-closes into the #3156 uncovered arm",
        )
        report(
            "AC1.order",
            guard != -1 and root != -1 and vo != -1 and guard < root and guard < vo and unc < root and unc < vo,
            "guard (+uncovered bump) ordered before the value-only root registration / bump",
        )
        report(
            "AC1.cite",
            cite != -1 and cite < arena_raw.find("public:", fn),
            "guard block cites Issue #4145 inside the auto_wire_ body",
        )
        # early return inside the guard: the guard branch must return before
        # the value-only arm (textual order: a `return;` between the guard
        # check and the root registration).
        report(
            "AC1.early_return",
            guard != -1 and root != -1 and ret != -1 and ret < root,
            "guard branch returns before the value-only registration (unreachable under required)",
        )
    else:
        report("AC1.guard", False, "auto_wire_ helper missing")
        report("AC1.order", False, "auto_wire_ helper missing")
        report("AC1.cite", False, "auto_wire_ helper missing")
        report("AC1.early_return", False, "auto_wire_ helper missing")

    # AC2 — exactly one live call site: the Soft/Off fallback, behind the
    # with_cover_ required arm.
    sites = 0
    first_at = -1
    at = arena.find(AUTO_WIRE_CALL)
    while at != -1:
        if first_at == -1:
            first_at = at
        sites += 1
        at = arena.find(AUTO_WIRE_CALL, at + 1)
    report(
        "AC2.single_site",
        sites == 1,
        f"exactly one live auto_wire_ call site (found {sites})",
    )
    wc = arena.find("void note_intermediate_create_with_cover_")
    req_arm = arena.find("general_object_pin_required_active()", wc) if wc != -1 else -1
    report(
        "AC2.required_arm_first",
        wc != -1 and req_arm != -1 and first_at != -1 and req_arm < first_at,
        "with_cover_ required arm precedes the Soft fallback call site",
    )

    # AC3 — soak SSOT unchanged; no second model.
    report(
        "AC3.pre_move_clause",
        arena.find("intermediate_create_value_only_total_v_read() > 0") != -1,
        "#3306 pre-move soak OR-clause intact",
    )
    report(
        "AC3.counter_export",
        arena_raw.find("g_intermediate_create_value_only_total{0}") != -1
        and arena_raw.find("g_intermediate_create_uncovered_under_required_total{0}") != -1,
        "value-only + uncovered counter exports unchanged (SSOT)",
    )
    report(
        "AC3.soft_arm_retained",
        fn != -1 and vo != -1,
        "Soft value-only bump retained behind the guard (zero-cost Soft contract)",
    )
    report(
        "AC3.no_second_model",
        arena_raw.find("g_soft_autowire") == -1 and arena_raw.find("note_intermediate_create_auto_wire_strict_") == -1,
        "no second auto-wire registry / helper invented",
    )

    # AC4 — runtime doors + wiring.
    ac_calls = [
        "ac4145_1_required_value_only_unreachable_fail_closed();",
        "ac4145_2_soft_value_only_contract_retained();",
        "ac4145_3_guard_source_cite_unreachable();",
        "ac4145_4_single_residual_site_and_wiring();",
    ]
    acs_defined = all(f"static void {c.removesuffix('();')}()" in tst for c in ac_calls)
    acs_dispatched = all(tst.count(c) == 1 for c in ac_calls)
    no_test_file = not (ROOT / "tests" / "core" / "test_issue_4145.cpp").exists()
    no_doc = not any((ROOT / "docs" / "design").glob("4145-*")) if (ROOT / "docs" / "design").exists() else True
    report(
        "AC4.tests",
        "#4145" in tst and acs_defined and acs_dispatched,
        "test_moving_densify_fail_closed.cpp cites #4145 with dispatched ac4145_1..4",
    )
    report(
        "AC4.no_invent",
        no_test_file and no_doc,
        "no tests/core/test_issue_4145.cpp; no docs/design/4145-*",
    )
    report(
        "AC4.wiring",
        "check_soft_autowire_unreachable_4145.py" in build and "check_soft_autowire_unreachable_4145.py" in allow,
        "build.py wires this linter; root_check_allowlist.txt lists it",
    )

    print(f"check_soft_autowire_unreachable_4145: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
