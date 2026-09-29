#!/usr/bin/env python3
"""Issue #4150 source-cite gate: Agent string/query face prefers the fiber.

hygiene_last_limit_reason_string() and the query:macro-hygiene-stats key
last-hygiene-limit-reason (schema-3029) still read the process-global
last-writer-wins atomic g_macro_hygiene_last_limit_reason while the
per-fiber FiberHygieneStats.last_limit_reason is the production
expand-deny authority (aura_hygiene_expand_deny_blocks_eval, #4034).
Under concurrent fibers cloning/mutating, fiber A's gensym-ceiling /
steal-abort / capability-deny can be overwritten by fiber B before Agent
dashboards / replay tooling sample the string — mis-attribution of
self-evo / hygiene incidents. This is a dropped correlation marker
between the enforce surface (fiber) and the Agent string/query face
(process), not "TLS depth as authority": depth stays the explicit
parameter and s_hygiene_depth remains observe-only.

ACs:
  AC1  hygiene_last_limit_reason_string() rides the fiber-preferring
       effective read (this fiber's slot first, process atomic as the
       slot-0 fallback) and cites #4150; the string face / key names are
       unchanged (no rename) and Soft/Off stays plain reads.
  AC2  aura_macro_hygiene_last_limit_reason_v_read() returns the same
       fiber-preferring effective read and cites #4150 (the query face
       keeps feeding from the bridge — same key name).
  AC3  the effective helper consults this fiber's FiberHygieneStats slot
       quietly under g_fiber_hygiene_mu (no get_fiber_hygiene_metrics
       query-counter bump, mirroring the #4078 blocks_eval consult) and
       falls back to the process atomic when the slot is 0 — the #4149
       mutate capability-deny stamps only the process sentinel, so the
       fallback must stay.
  AC4  the enforce face is untouched: aura_hygiene_expand_deny_blocks_eval
       still reads the fiber map under lock (fiber-authoritative,
       #4034/#4078) and inner_expand_production_limit_deny still consults
       get_fiber_hygiene_metrics — the Agent face change must not flip
       the authority split.
  AC5  the query key is unchanged: evaluator_primitives_query_obs_mid.cpp
       still inserts "last-hygiene-limit-reason" fed by
       aura_macro_hygiene_last_limit_reason_v_read() (schema-3029 keys
       intact; query:macro-hygiene-provenance-stats not renamed).
  AC6  tests cite #4150 (runtime ACs in test_hygiene_mutate_closed_loop);
       no tests/**/test_issue_4150.cpp; no docs/design/4150-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MCX = ROOT / "src" / "compiler" / "macro_expansion.cpp"
QOM = ROOT / "src" / "compiler" / "evaluator_primitives_query_obs_mid.cpp"
TST = ROOT / "tests" / "compiler" / "test_hygiene_mutate_closed_loop.cpp"
BUILD = ROOT / "build.py"
ALLOW = ROOT / "scripts" / "coverage" / "root_check_allowlist.txt"


def _slice(text: str, start: str, end: str) -> str:
    i = text.find(start)
    if i < 0:
        return ""
    j = text.find(end, i)
    return text[i:j] if j > i else ""


def main() -> int:
    fails: list[str] = []

    mcx = MCX.read_text()

    # AC1: the Agent string face rides the fiber-preferring effective read.
    sfn = _slice(
        mcx,
        "const char* hygiene_last_limit_reason_string() noexcept {",
        "hygiene_limit_reason_string_for(hygiene_last_limit_reason_effective());",
    )
    if not sfn:
        fails.append("AC1: hygiene_last_limit_reason_string does not ride the effective read")
    if "#4150" not in sfn:
        fails.append("AC1: string face does not cite #4150")
    if "g_macro_hygiene_last_limit_reason.load" in sfn:
        fails.append("AC1: string face still reads the process atomic directly")

    # AC2: the v_read bridge is fiber-preferring too (query face).
    vrd = _slice(
        mcx,
        'extern "C" std::uint64_t aura_macro_hygiene_last_limit_reason_v_read(void) noexcept {',
        "hygiene_last_limit_reason_effective();",
    )
    if not vrd:
        fails.append("AC2: aura_macro_hygiene_last_limit_reason_v_read is not fiber-preferring")
    if "#4150" not in vrd:
        fails.append("AC2: v_read bridge does not cite #4150")
    if "g_macro_hygiene_last_limit_reason.load" in vrd:
        fails.append("AC2: v_read bridge still reads the process atomic directly")

    # AC3: the effective helper — quiet fiber consult + process fallback.
    # Slice inclusively (the fallback line is the tail anchor) and cite
    # via the definition comment block, which sits above the signature.
    eff_sig = "[[nodiscard]] static std::uint8_t hygiene_last_limit_reason_effective() noexcept {"
    fallback = "return g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed);"
    _i = mcx.find(eff_sig)
    _j = mcx.find(fallback, _i) if _i >= 0 else -1
    eff = mcx[_i : _j + len(fallback)] if (_i >= 0 and _j >= 0) else ""
    if not eff:
        fails.append("AC3: hygiene_last_limit_reason_effective helper missing")
    if "g_fiber_hygiene_mu" not in eff or "g_fiber_hygiene_map.find(fid)" not in eff:
        fails.append("AC3: helper does not consult this fiber's FiberHygieneStats slot")
    if "get_fiber_hygiene_metrics(" in eff:
        fails.append("AC3: helper must stay quiet (get_fiber_hygiene_metrics bumps the query counter)")
    if not eff.endswith(fallback):
        fails.append("AC3: helper missing the slot-0 process-atomic fallback (#4149 boundary)")
    if (
        "// Issue #4150: fiber-preferring effective read for the Agent string/query\n"
        "// face. Per-fiber FiberHygieneStats.last_limit_reason is the expand-deny"
    ) not in mcx:
        fails.append("AC3: helper does not cite #4150")

    # AC4: the enforce face stays fiber-authoritative.
    blk = _slice(
        mcx,
        'extern "C" int aura_hygiene_expand_deny_blocks_eval(void) noexcept {',
        "return fr == kHygieneLimitReasonDepthLimit",
    )
    if "g_fiber_hygiene_map.find(fid)" not in blk:
        fails.append("AC4: aura_hygiene_expand_deny_blocks_eval no longer reads the fiber map (#4034)")
    if "g_macro_hygiene_last_limit_reason.load" in blk:
        fails.append("AC4: blocks_eval must not consult the process atomic (fiber is the authority)")
    deny_fn = _slice(
        mcx,
        "[[nodiscard]] bool inner_expand_production_limit_deny() noexcept {",
        "fr == kHygieneLimitReasonGensymCeiling;",
    )
    if "get_fiber_hygiene_metrics(fid).last_limit_reason" not in deny_fn:
        fails.append("AC4: inner_expand_production_limit_deny fiber consult missing (#4034)")

    # AC5: the query key is unchanged and still fed by the bridge.
    qom = QOM.read_text()
    kv = _slice(
        qom,
        'insert_kv("last-hygiene-limit-reason",',
        "static_cast<std::int64_t>(aura_macro_hygiene_last_limit_reason_v_read()));",
    )
    if not kv:
        fails.append("AC5: query key last-hygiene-limit-reason not fed by the v_read bridge")
    if '"query:macro-hygiene-provenance-stats"' not in _slice(
        (ROOT / "src" / "compiler" / "evaluator_primitives_obs_jit.cpp").read_text(),
        '"query:macro-hygiene-provenance-stats"',
        "build_hash",
    ):
        fails.append("AC5: query:macro-hygiene-provenance-stats registration missing (no rename)")

    # AC6: tests + wiring + no artifacts.
    tst = TST.read_text()
    if tst.count("#4150") < 5:
        fails.append("AC6: test file does not cite #4150 (runtime ACs missing)")
    if (ROOT / "tests" / "compiler" / "test_issue_4150.cpp").exists():
        fails.append("AC6: tests/compiler/test_issue_4150.cpp must not exist (#81934)")
    if any((ROOT / "docs" / "design").glob("4150-*")):
        fails.append("AC6: docs/design/4150-* must not exist (#1655)")
    if "check_hygiene_last_limit_fiber_4150" not in BUILD.read_text():
        fails.append("AC6: build.py does not wire check_hygiene_last_limit_fiber_4150.py")
    if "check_hygiene_last_limit_fiber_4150.py" not in ALLOW.read_text():
        fails.append("AC6: scripts/coverage/root_check_allowlist.txt does not list the linter")

    if fails:
        for f in fails:
            print(f"FAIL check_hygiene_last_limit_fiber_4150: {f}", file=sys.stderr)
        return 1
    print("check_hygiene_last_limit_fiber_4150: OK (6 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
