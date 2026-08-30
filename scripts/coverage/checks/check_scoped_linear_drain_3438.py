#!/usr/bin/env python3
"""Issue #3438: scoped linear-root drain on the three live-fiber faces.

unpin_all_linear_roots() was process-wide at Fiber::release_orphan_roots
(post-join reclaim), Evaluator::enforce_linear_post_failure (outermost
Guard fail), and the steal-complete hard-fail arm — wiping sibling
fibers' live linear roots (false-green Moving verify / UAF under
multi-fiber mutate).

Fix: the outermost MutationBoundaryGuard enter (production-gated, same
arm as nested_linear_keep_) snapshots live linear roots onto the Fiber
(set_outermost_linear_keep). The three drain faces share ONE audit face
— aura::serve::unpin_linear_roots_scoped_for_fiber(fiber): armed keep
-> unpin_linear_roots_except(keep) (siblings survive; take+disarm),
else legacy unpin_all fallback (#3023 standalone / Soft contract). The
successful outermost exit clears the fiber keep (stale-keep guard).
unpin_all_linear_roots stays for process teardown /
reset_linear_roots_for_test only.

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

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

    fh = _read("src/serve/fiber.h")
    fc = _read("src/serve/fiber.cpp")
    gc = _read("src/compiler/evaluator_gc.cpp")
    st = _read("src/compiler/evaluator_fiber_mutation.cpp")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    test = _read("tests/serve/test_fiber_reclaim_orphan_release.cpp")
    build = _read("build.py")

    # AC1/AC3: fiber face — shared scoped helper + keep storage on the Fiber.
    must("unpin_linear_roots_scoped_for_fiber", "AC3 helper decl", fh)
    must("unpin_linear_roots_scoped_for_fiber", "AC3 helper impl", fc)
    must("set_outermost_linear_keep", "AC1 keep setter", fh)
    must("take_outermost_linear_keep", "AC1 keep taker", fh)
    i = fc.find("std::size_t Fiber::release_orphan_roots() noexcept")
    if i < 0:
        fails.append("AC1: release_orphan_roots not found")
    else:
        end = fc.find("return n;", i)
        snip = fc[i : (end + 15 if end > 0 else i + 900)]
        must("unpin_linear_roots_scoped_for_fiber(this)", "AC1 scoped call", snip)
        must("Issue #3438", "AC1 cite", snip)
        if "unpin_all_linear_roots" in snip:
            fails.append("AC1: release still references unpin_all")

    # AC2: outermost-fail face scoped.
    j = gc.find("Evaluator::enforce_linear_post_failure(std::uint8_t path)")
    if j < 0:
        fails.append("AC2: enforce not found")
    else:
        snip = gc[j : j + 2400]
        must("unpin_linear_roots_scoped_for_fiber", "AC2 scoped call", snip)
        must("Issue #3438", "AC2 cite", snip)

    # AC3: steal hard-fail face scoped (same helper).
    must("unpin_linear_roots_scoped_for_fiber(fiber)", "AC3 scoped steal drain", st)
    k = st.find("unpin_linear_roots_scoped_for_fiber(fiber)")
    if k >= 0:
        must("Issue #3438", "AC3 cite", st[max(0, k - 700) : k + 200])

    # Guard wiring: outermost enter publish + exit clear (production-gated).
    must("set_outermost_linear_keep", "wiring publish", mb)
    must("clear_outermost_linear_keep", "wiring clear", mb)
    b = mb.find("if (outermost && typed_audit::production_defaults_active())")
    if b >= 0:
        must("Issue #3438", "wiring cite", mb[max(0, b - 600) : b + 900])
    else:
        fails.append("wiring: outermost production arm not found")

    # AC5: no per-fiber pin registry / no second model.
    for src_name, src in (("fiber.h", fh), ("fiber.cpp", fc)):
        if "linear_root_registry" in src or "g_fiber_pin_table" in src:
            fails.append(f"AC5: second registry surface in {src_name}")

    # AC6: src-aligned suite extension, no test_issue file.
    must("ac6_3438_scoped_linear_drain", "AC6 test fn", test)
    must("sibling linear root survives the scoped reclaim drain", "AC6 label", test)
    must("unarmed fallback still drains", "AC5 fallback label", test)
    for forbidden in ("tests/serve/test_issue_3438.cpp", "tests/issues/test_issue_3438.cpp"):
        if (ROOT / forbidden).is_file():
            fails.append(f"AC6: {forbidden} present")

    # Wiring in build.py after #3437.
    must("check_scoped_linear_drain_3438", "wiring build.py", build)
    prev = build.find("check_scope_session_drop_3437")
    ours = build.find("check_scoped_linear_drain_3438")
    if ours < 0:
        fails.append("wiring: linter not in build.py")
    elif prev >= 0 and ours < prev:
        fails.append("wiring: must come after #3437 linter")

    if fails:
        print("FAIL: Issue #3438 scoped linear drain:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3438 scoped linear drain — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
