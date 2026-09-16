#!/usr/bin/env python3
"""Issue #3844: grant/SE epoch never invents phantom 1 under hard face.

Residual: check_and_record_effect / make_grant_provenance / grant_macro_self_evo
/ check_macro_self_evo stamped prov.epoch = me ?: 1 when WorkspaceEpoch
Mutation was 0. Under production_defaults || Full that phantom 1 false-joins
SE/WAL/grant epoch to Mutation while mid-join can look healthy.

Contract (one row per AC):
  AC1  Hard face (production_defaults || Full): stamp_grant_mutation_epoch /
       make_grant_provenance / check_and_record_effect keep epoch=0
  AC2  Soft observe stamp (epoch=1) retained and documented
  AC3  Extends test_audit_mutation_id_unify; linter + grandfather +
       manifest + build.py; not a dup of #3837; no invent / docs/design

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SEC = "src/compiler/evaluator_security.cpp"
CAP = "src/core/capability_model.hh"
HOOKS = "src/compiler/typed_mutation_audit_hooks.cpp"
TEST = "tests/compiler/test_audit_mutation_id_unify.cpp"
BUILD = "build.py"
GF = "scripts/coverage/simple_check_grandfather.txt"
MANIFEST = "scripts/coverage/manifests/3844.json"
LINTER = "check_grant_epoch_no_phantom_3844"


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
            fails.append(f"{label}: forbidden {n!r} present")

    sec = _read(SEC)
    cap = _read(CAP)
    hooks = _read(HOOKS)
    test = _read(TEST)
    build = _read(BUILD)
    gf = _read(GF)
    man = _read(MANIFEST)

    # AC1 — hard face keeps epoch 0; helpers + call sites patched.
    must("Issue #3844", "AC1 cite sec", sec)
    must("Issue #3844", "AC1 cite cap", cap)
    must("stamp_grant_mutation_epoch", "AC1 stamp helper", cap)
    must("capability_epoch_hard_face", "AC1 hard-face helper", cap)
    must("aura_production_hard_face_active_probe", "AC1 probe decl", cap)
    must("aura_production_hard_face_active_probe", "AC1 probe def", hooks)
    must("production_hard_face_active()", "AC1 hooks use helper", hooks)

    care = sec.find("bool Evaluator::check_and_record_effect")
    if care < 0:
        fails.append("AC1: check_and_record_effect not found")
        care_win = ""
    else:
        care_win = sec[care : care + 6000]
    must("production_hard_face_active()", "AC1 care hard gate", care_win)
    must("prov.epoch = me; // 0 stays 0", "AC1 care keep 0", care_win)
    must("Soft observe stamp only", "AC2 Soft doc in care", care_win)

    must("prov.epoch = stamp_grant_mutation_epoch()", "AC1 make_grant uses stamp", cap)
    # grant_macro_self_evo + check_macro_self_evo use stamp helper
    if cap.count("stamp_grant_mutation_epoch()") < 3:
        fails.append(
            f"AC1: expected ≥3 stamp_grant_mutation_epoch() call sites, got {cap.count('stamp_grant_mutation_epoch()')}"
        )
    # record_audit must not fall through epoch → mid
    must("const auto epoch = prov.epoch;", "AC1 record_audit epoch=prov.epoch", cap)
    must_not(
        "const auto epoch = prov.epoch != 0 ? prov.epoch : mid;",
        "AC1 no epoch?:mid invent",
        cap,
    )

    # AC2 Soft arm retained in stamp helper
    must("Soft observe", "AC2 Soft observe comment", cap)
    soft_helper = cap[cap.find("stamp_grant_mutation_epoch") : cap.find("stamp_grant_mutation_epoch") + 500]
    must("me != 0 ? me : static_cast<std::uint64_t>(1)", "AC2 Soft invent arm", soft_helper)

    # AC3 suite + wiring + no invent; not a dup of #3837
    must("ac3844_1_hard_epoch0_stays_zero", "AC3 test AC1", test)
    must("ac3844_2_soft_observe_stamp_documented", "AC3 test AC2", test)
    must("ac3844_3_source_cite_wiring_no_invent", "AC3 test AC3", test)
    must("Issue #3844", "AC3 test cite", test)
    must("#3837", "AC3 not-dup-of-3837 cite", test + sec + cap)
    must(LINTER, "AC3 build registration", build)
    must("Issue #3844", "AC3 build cite", build)
    must(f"{LINTER}.py", "AC3 grandfather basename", gf)
    must(f"scripts/coverage/checks/{LINTER}.py", "AC3 grandfather path", gf)
    must('"issue": 3844', "AC3 manifest issue", man)
    if not (ROOT / MANIFEST).is_file():
        fails.append(f"AC3: {MANIFEST} missing")
    for rel in (
        "tests/compiler/test_issue_3844.cpp",
        "tests/core/test_issue_3844.cpp",
        "tests/issues/test_issue_3844.cpp",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC3: forbidden invent {rel}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        for p in design.glob("3844*"):
            fails.append(f"AC3: docs/design/{p.name} exists")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    qr = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    must_not("schema-3844", "AC3 no new schema key", q + qr)
    must_not("query:grant-epoch-no-phantom", "AC3 no new query key", q + qr)

    if fails:
        for f in fails:
            print(f"FAIL {LINTER}: {f}", file=sys.stderr)
        print(f"\n{len(fails)} check(s) failed for #3844", file=sys.stderr)
        return 1

    print(f"OK {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
