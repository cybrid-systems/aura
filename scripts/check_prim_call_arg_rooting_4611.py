#!/usr/bin/env python3
"""Issue #4611 source-cite gate (cached prim-call argument rooting).

The tree-walker's cached primitive-call fast path (try_eval_flat_hot, the
#4599 per-node PrimCall cache) evaluates a call's arguments into a stack
buffer. The buffer was registered as a GC root only AFTER the argument
loop, so an argument that was already evaluated was unrooted while a
later argument was evaluated. The nested eval reaches the top of its own
TCO loop, where the periodic file-mode reclaim
(maybe_reclaim_tw_unreachable -> reclaim_tw_unreachable) marks and sweeps
the heap. An earlier argument's freshly minted pair was not reachable
from any root (env frame / pinned vector / pinned result), so the sweep
freed it and the call continued with a dangling pair.

Visible face: (filter (lambda (i) (= (car i) 7)) items) over a
named-let-built list read freed memory and reported
"type mismatch - expected Int, got Pair" once the list was long enough
for the reclaim to fire mid-build (the #4459 128-allocation floor).

The fix pins the argument buffer BEFORE the argument loop and carries a
LIVE root count, so the rooted prefix grows with the buffer.

ACs:
  AC1  evaluator_eval_flat.cpp: TwSpanRoot carries a live count pointer
       (n_live) plus the fixed-count fallback, and exposes count().
  AC2  evaluator_eval_flat.cpp: TwPinSpan gains the live-count pin (the
       Live tag constructor) next to the fixed-count pin.
  AC3  evaluator_eval_flat.cpp: BOTH root collectors (the reclaim's
       maybe_reclaim_tw_unreachable and the IR-side
       collect_tw_inflight_roots) read the live count.
  AC4  evaluator_eval_flat.cpp: the cached prim-call path pins the
       argument buffer before the argument loop, publishes each
       evaluated argument (buf_n = i + 1), and no longer pins the
       buffer after the loop.
  AC5  tests/compiler/test_pmr_alloc_fiber_safe.cpp:
       ac4611_prim_call_arg_rooting exists and is registered in
       run_test_pmr_alloc_fiber_safe; the 1000-element named-let filter
       over (car i) with = and <, the pair? face, the intact-list face
       and the reclaim-ran face are pinned at runtime. No
       tests/issues/** for this issue, no docs/design/4611-* (per #1655).
  AC6  Wiring: build.py registers this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EFL = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
TST = ROOT / "tests" / "compiler" / "test_pmr_alloc_fiber_safe.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"

failures: list[str] = []


def must(cond: bool, label: str) -> None:
    if cond:
        print(f"  ok  {label}")
    else:
        failures.append(label)
        print(f"FAIL  {label}")


def _between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i + len(start))
    return text[i : j if j > 0 else len(text)]


def _squeeze(text: str) -> str:
    """Drop whitespace and quote characters.

    clang-format may split a long expression or a long string literal over
    several lines, so a source-cite row must not depend on the exact line
    breaks. Squeezing keeps the token sequence and drops only layout.
    """
    return re.sub(r'[\s"]+', "", text)


def has(text: str, needle: str) -> bool:
    return _squeeze(needle) in _squeeze(text)


def main() -> int:
    efl = EFL.read_text()
    tst = TST.read_text()
    build = BUILD.read_text()
    allow = ALLOW.read_text()

    # ── AC1: the span root carries a live count ──────────────────────
    span_root = _between(efl, "struct TwSpanRoot {", "thread_local std::vector<TwSpanRoot> g_tw_spans;")
    must(bool(span_root), "AC1: TwSpanRoot block found")
    must(has(span_root, "const std::size_t* n_live = nullptr;"), "AC1: TwSpanRoot carries a live count pointer")
    must(has(span_root, "std::size_t n_fixed = 0;"), "AC1: TwSpanRoot keeps the fixed-count fallback")
    must(
        has(span_root, "return n_live != nullptr ? *n_live : n_fixed;"),
        "AC1: TwSpanRoot::count() reads the live count when present",
    )

    # ── AC2: the live-count pin ──────────────────────────────────────
    pin = _between(efl, "struct TwPinSpan {", "struct TwPinSpan(const TwPinSpan&) = delete;")
    must(bool(pin), "AC2: TwPinSpan block found")
    must(has(pin, "struct Live {};"), "AC2: TwPinSpan exposes the Live tag")
    must(
        has(pin, "TwPinSpan(Live, const types::EvalValue* data, const std::size_t* n_live)")
        and has(pin, "g_tw_spans.push_back(TwSpanRoot{data, n_live, 0});"),
        "AC2: the live-count pin registers the live count with the span root",
    )
    must(
        has(pin, "g_tw_spans.push_back(TwSpanRoot{data, nullptr, n});"),
        "AC2: the fixed-count pin keeps the fixed count",
    )

    # ── AC3: both root collectors read the live count ─────────────────
    reclaim = _between(
        efl,
        "void Evaluator::maybe_reclaim_tw_unreachable() {",
        "(void)reclaim_tw_unreachable(envs, held);",
    )
    must(bool(reclaim), "AC3: the reclaim root collector found")
    must(
        has(reclaim, "const auto n = sp.count();") and has(reclaim, "for (std::size_t i = 0; i < n; ++i)"),
        "AC3: the reclaim root collector reads the live count",
    )
    inflight = _between(
        efl,
        "void Evaluator::collect_tw_inflight_roots(std::vector<types::EvalValue>& out) {",
        "// Issue #4568: file and stdin eval never enter IRInterpreter::run_function.",
    )
    must(bool(inflight), "AC3: the IR-side root collector found")
    must(
        has(inflight, "const auto n = sp.count();") and has(inflight, "for (std::size_t i = 0; i < n; ++i)"),
        "AC3: the IR-side root collector reads the live count",
    )
    must(
        not has(efl, "for (std::size_t i = 0; i < sp.n; ++i)"),
        "AC3: no collector still reads the retired fixed-count field",
    )

    # ── AC4: the cached prim-call path roots its arguments ───────────
    prim = _between(
        efl,
        "const auto nargs = static_cast<std::size_t>(e.nchild - 1);",
        "// Issue #4524: the named-let step",
    )
    must(bool(prim), "AC4: the cached prim-call argument window found")
    must(has(prim, "std::array<types::EvalValue, 8> buf{};"), "AC4: the argument buffer is the stack array")
    pin_at = _squeeze(prim).find(_squeeze("TwPinSpan pin(TwPinSpan::Live{}, buf.data(), &buf_n);"))
    loop_at = _squeeze(prim).find(_squeeze("for (std::size_t i = 0; i < nargs; ++i) {"))
    must(pin_at >= 0, "AC4: the argument buffer is pinned with a live count")
    must(loop_at >= 0, "AC4: the argument loop found")
    must(
        pin_at >= 0 and loop_at >= 0 and pin_at < loop_at,
        "AC4: the pin is registered BEFORE the argument loop",
    )
    must(has(prim, "std::size_t buf_n = 0;"), "AC4: the live root counter is declared")
    must(
        has(prim, "buf_n = i + 1;"),
        "AC4: each evaluated argument is published to the root count",
    )
    must(
        not has(prim, "TwPinSpan pin(buf.data(), nargs);"),
        "AC4: the after-the-loop pin is gone",
    )

    # ── AC5: the runtime regression test ─────────────────────────────
    must(has(tst, "static void ac4611_prim_call_arg_rooting()"), "AC5: the runtime AC block exists")
    must(
        has(tst.split("int run_test_pmr_alloc_fiber_safe()")[1], "ac4611_prim_call_arg_rooting();"),
        "AC5: the AC block is registered in the dispatched runner",
    )
    must(
        has(tst, "4611 AC1: filter with = over (car i) on a 1000-element named-let list returns ((7))"),
        "AC5: the 1000-element named-let filter with = is pinned at runtime",
    )
    must(
        has(tst, "4611 AC2: filter with < over (car i) on a 1000-element named-let list returns ((2) (1) (0))"),
        "AC5: the 1000-element named-let filter with < is pinned at runtime",
    )
    must(
        has(tst, "4611 AC3: the pair? predicate reads no freed memory (keeps nothing)"),
        "AC5: the freed-memory read face is pinned at runtime",
    )
    must(
        has(tst, "4611 AC4: the named-let-built list stays intact (length / list-ref / map)"),
        "AC5: the intact-list face is pinned at runtime",
    )
    must(
        has(tst, "4611 AC5: the periodic reclaim fired during the 1000-element build"),
        "AC5: the reclaim-ran face proves the window was exercised",
    )
    must(
        not (ROOT / "tests" / "issues").exists()
        or not any("4611" in p.name for p in (ROOT / "tests" / "issues").iterdir()),
        "AC5: no tests/issues/ entry for this issue",
    )
    must(
        not (ROOT / "docs" / "design" / "4611-prim-call-arg-rooting.md").exists(),
        "AC5: no docs/design/4611-* (per #1655)",
    )

    # ── AC6: wiring ──────────────────────────────────────────────────
    must("check_prim_call_arg_rooting_4611.py" in build, "AC6: build.py registers the linter")
    must("check_prim_call_arg_rooting_4611.py" in allow, "AC6: root_check_allowlist.txt lists the linter")

    if failures:
        print(f"\n{len(failures)} row(s) failed")
        return 1
    print("\nall rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
