#!/usr/bin/env python3
"""Issue #4167: production CMake face defaults to AURA_PRODUCTION_PACK.

Non-PACK production (NDEBUG without the define) compiled the #3501 OFF
expansion: every hot AURA_HOT_CONTRACT (value.ixx as_* / ir_soa.ixx
view_at) paid a relaxed hot_contract_harden_armed_cache load per call,
and with defaults inactive the predicate was NOT evaluated — the Quiet
OOB ship-without-arm window (#3866 refuses it at multi-worker Ready; the
face default closes it at configure time). The aura target now receives
the define behind an option that DEFAULTS ON; Soft/unit test targets
keep the #3490/#3501 runtime cache (#3666 AC2 / #3313 AC2), and the
#3666/#3702 pack semantics (const-armed Harden, [[unlikely]] abort only,
no happy-path RECORD) are reused verbatim — no second contract system.

Contract (one row per AC):
  AC1  CMake face default: option(AURA_PRODUCTION_PACK ...) default ON,
       aura define sits behind if(AURA_PRODUCTION_PACK), #4167 cite
  AC2  exactly two AURA_PRODUCTION_PACK=1 target rows (aura + the #3627
       pack mirror fixture); no other target receives the define
  AC3  pack face intact: OFF+PACK window aborts + traps, no armed() /
       cache load, unconditional predicate, no sampled RECORD; view_at /
       as_int call sites keep AURA_HOT_CONTRACT
  AC4  Soft/unit keep: hot_contract_harden_armed_cache +
       note_hot_contract_harden_armed + "expr not evaluated"; no new
       header stamp / mode / query key
  AC5  fixture ACs wired (ac4167_pack_face_default), linter in build.py
       gate + root allowlist; no docs/design; no test_issue_4167.cpp

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    cm = _read("CMakeLists.txt")
    hh = _read("src/core/cpp26_contract_stats.h")
    val = _read("src/compiler/value.ixx")
    soa = _read("src/compiler/ir_soa.ixx")
    fixture = _read("tests/compiler/test_pack_pipeline_strict.cpp")
    build = _read("build.py")
    allow = _read("scripts/coverage/root_check_allowlist.txt")

    # AC1: the production face defaults ON.
    must("Issue #4167", "AC1 cmake cite", cm)
    must("option(AURA_PRODUCTION_PACK", "AC1 option present", cm)
    must("default ON, #4167", "AC1 option default ON", cm)
    opt = cm.find("option(AURA_PRODUCTION_PACK")
    opt_end = cm.find("endif()", opt) if opt >= 0 else -1
    win = cm[opt:opt_end] if opt >= 0 and opt_end > opt else ""
    must("if(AURA_PRODUCTION_PACK)", "AC1 aura define behind option", win)
    must("target_compile_definitions(aura PRIVATE AURA_PRODUCTION_PACK=1)", "AC1 aura define", win)
    must_not(
        "target_compile_definitions(aura PRIVATE AURA_PRODUCTION_PACK=1)",
        "AC1 define not outside the option block",
        cm[:opt] + cm[opt_end:],
    )

    # AC2: exactly two rows carry the define; Soft/unit stay non-pack.
    if cm.count("PRIVATE AURA_PRODUCTION_PACK=1") != 2:
        fails.append(f"AC2: PRIVATE AURA_PRODUCTION_PACK=1 rows {cm.count('PRIVATE AURA_PRODUCTION_PACK=1')} != 2")
    must(
        "target_compile_definitions(test_pack_pipeline_strict PRIVATE AURA_PRODUCTION_PACK=1)",
        "AC2 pack mirror fixture kept",
        cm,
    )
    must_not(
        "target_compile_definitions(aura_test_objects PRIVATE AURA_PRODUCTION_PACK=1)",
        "AC2 aura_test_objects stays Soft",
        cm,
    )

    # AC3: pack semantics intact (reuse #3666/#3702 — no second system).
    guard = hh.find("#if defined(AURA_HOT_MODE_OFF) && defined(AURA_PRODUCTION_PACK)")
    if guard < 0:
        fails.append("AC3: missing pack OFF redefine guard")
        pwin = ""
    else:
        end = hh.find("#endif", guard)
        pwin = hh[guard:end] if end > guard else ""
    must("std::abort()", "AC3 pack abort", pwin)
    must("record_hotpath_contract_harden_trap", "AC3 pack trap", pwin)
    must("[[unlikely]]", "AC3 pack unlikely", pwin)
    must_not("hot_contract_harden_armed()", "AC3 pack no armed() gate", pwin)
    must_not("hot_contract_harden_armed_cache", "AC3 pack no cache load", pwin)
    cpos = pwin.find("#define AURA_HOT_CONTRACT")
    cbody = pwin[cpos:] if cpos >= 0 else ""
    must("#define AURA_HOT_CONTRACT", "AC3 pack CONTRACT define", pwin)
    must_not("record_hotpath_invariant_hit_sampled", "AC3 pack CONTRACT no happy RECORD", cbody)
    must("AURA_HOT_CONTRACT(is_int(v))", "AC3 as_int CONTRACT", val)
    vat = soa.find("IRInstructionView view_at(")
    addb = soa.find("add_block", vat) if vat >= 0 else -1
    vwin = soa[vat:addb] if vat >= 0 and addb > vat else ""
    must("AURA_HOT_CONTRACT", "AC3 view_at CONTRACT", vwin)

    # AC4: Soft/unit runtime cache kept; no new mode / stamp / query key.
    must("hot_contract_harden_armed_cache", "AC4 Soft cache kept", hh)
    must("note_hot_contract_harden_armed", "AC4 cache store kept", hh)
    must("expr not evaluated", "AC4 unarmed NDEBUG-OFF skip kept", hh)
    must_not("kHotContractPackFaceDefaultIssue", "AC4 no new header stamp", hh)
    must_not("schema-4167", "AC4 no new query key", hh)

    # AC5: fixture ACs + gate wiring + allowlist; no invent / docs.
    must("void ac4167_pack_face_default()", "AC5 fixture AC fn", fixture)
    must("ac4167_pack_face_default();", "AC5 fixture main call", fixture)
    must("4167 AC3: option defaults ON", "AC5 fixture AC label", fixture)
    must("check_hot_contract_pack_face_4167", "AC5 build.py wiring", build)
    must("check_hot_contract_pack_face_4167.py", "AC5 allowlist entry", allow)
    if _read("docs/design/4167-production-pack-face-default.md"):
        fails.append("AC5: docs/design/4167-* exists")
    if _read("tests/compiler/test_issue_4167.cpp"):
        fails.append("AC5: test_issue_4167.cpp exists")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        print(f"check_hot_contract_pack_face_4167: {len(fails)} row(s) failed")
        return 1
    print("check_hot_contract_pack_face_4167: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
