#!/usr/bin/env python3
"""Issue #4120 source-cite gate: empty-arg replay-mid uses last_stamped.

query:capability-effect-stats (#3143 replay face) defaulted the empty /
non-int replay-mid via last_type_linear_commit_proof_stamp_v_read() (then
current_mutation_epoch()). #4098 / #3778: the TypeLinear proof stamp is NOT
the session / TypedMid join key (steal/resume / boundary TLS clears) — an
empty-arg replay could therefore follow a diverged proof/epoch and report
se-count / verdict rows for a mid the SE/grant/trail never carried (the
sibling query:evolution-audit-decision already defaults to
g_last_stamped_audit_mid, #3284/#3738).

ACs:
  AC1  evaluator_primitives_security.cpp: the capability-effect-stats
       replay block splits explicit int args from omitted / non-int
       (filt_mid); omitted / non-int reads g_last_stamped_audit_mid
       (relaxed load) then the mutation epoch; the proof stamp is no
       longer consulted in the default chain; cites #4120.
  AC2  typed-mid-current still publishes the proof stamp via
       last_type_linear_commit_proof_stamp_v_read() — the typed-face
       observability key is kept, not removed (#4120 AC2).
  AC3  Explicit mid=0 stays a legal refuse-class filter (parity #3462 /
       #3738): the filt_mid path assigns the raw argument with no
       zero-replacement; no new query:* key, no new insert_kv row, no
       hash-capacity change (the fix adds none — #3339/#3971 headroom
       untouched); schema-3143 surface unchanged.
  AC4  tests/core/test_audit_replay_join.cpp extends the existing
       audit-replay family with the diverged-stamp repro (proof stamp
       999999 ≠ last_stamped 4120001; empty/non-int arg → last_stamped;
       explicit 0 stays 0; cleared faces → 0, no phantom mid=1; SE join on
       S) and is registered in run_test_audit_replay_join; no
       tests/**/test_issue_4120.cpp (per #81934); no docs/design/4120-*;
       build.py wires this linter and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_security.cpp"
TST = ROOT / "tests" / "core" / "test_audit_replay_join.cpp"


def main() -> int:
    prim = PRIM.read_text() if PRIM.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    # Region of the #3143 replay block: from the surface registration to
    # the replay-mid insert (the typed-mid-current proof-stamp read sits
    # AFTER this region — region-scoped absence is the contract).
    reg_at = prim.find("query:capability-effect-stats")
    rm_at = prim.find('insert_kv("replay-mid"')
    region = prim[reg_at:rm_at] if reg_at >= 0 and rm_at > reg_at else ""

    # AC1 — default chain: filt_mid split, last_stamped SSOT, no proof stamp.
    good = (
        "#4120" in region
        and "const bool filt_mid = !args.empty() && is_int(args[0]);" in region
        and "g_last_stamped_audit_mid.load(" in region
        and "std::memory_order_relaxed" in region
        and "current_mutation_epoch()" in region
        and region.count("last_type_linear_commit_proof_stamp_v_read") == 0
    )
    report(
        "AC1",
        good,
        "replay block: omitted/non-int → g_last_stamped_audit_mid (relaxed) then epoch; proof stamp not consulted; cites #4120",
    )

    # AC2 — typed-mid-current keeps the proof stamp.
    tail = prim[rm_at:] if rm_at >= 0 else ""
    good = (
        'insert_kv("typed-mid-current"' in tail
        and "last_type_linear_commit_proof_stamp_v_read()" in tail
        and 'insert_kv("replay-mid"' in prim
    )
    report(
        "AC2",
        good,
        "typed-mid-current still publishes last_type_linear_commit_proof_stamp_v_read() (observability key kept)",
    )

    # AC3 — explicit 0 refuse-class preserved; no new keys / capacity.
    good = (
        "if (filt_mid)" in region
        and "mid = static_cast<std::uint64_t>(as_int(args[0]));" in region
        and "#3738" in region
        and "#3462" in region
        # No new query key / metrics bus / insert_kv row (additive-key
        # freeze: the fix adds none, hash capacity 186 untouched).
        and 'insert_kv("issue-4120"' not in prim
        and 'insert_kv("schema-4120"' not in prim
        and "query:replay-mid" not in prim.replace('insert_kv("replay-mid"', "")
        and 'insert_kv("schema-3143"' in prim
        and "query:capability-effect-stats" in prim
    )
    report(
        "AC3",
        good,
        "explicit mid=0 stays refuse-class (#3462/#3738); no new query key / insert_kv row / capacity",
    )

    # AC4 — runtime doors + registration + no invented files.
    no_test_file = not any(p.name == "test_issue_4120.cpp" for p in (ROOT / "tests").rglob("*.cpp"))
    design_dir = ROOT / "docs" / "design"
    no_doc = not any(design_dir.glob("4120-*")) if design_dir.exists() else True
    good = (
        "ac21_replay_mid_last_stamped_4120" in tst
        and "ac21_replay_mid_last_stamped_4120();" in tst
        and tst.count("4120 AC1") >= 2
        and tst.count("4120 AC2") >= 1
        and tst.count("4120 AC3") >= 1
        and tst.count("4120 AC4") >= 1
        and tst.count("4120 AC5") >= 2
        and "4120001" in tst
        and "999999" in tst
        and "stamp_type_linear_commit_proof(999999)" in tst
        and "query:capability-effect-stats\\" in tst.replace("\\\\", "\\")
        and no_test_file
        and no_doc
        and "check_replay_mid_last_stamped_4120.py" in build
        and "check_replay_mid_last_stamped_4120.py" in allow
    )
    report(
        "AC4",
        good,
        "test_audit_replay_join.cpp hosts the diverged-stamp repro (ac21), runner-registered; no test_issue_4120.cpp / docs; build.py + allowlist wired",
    )

    print("check_replay_mid_last_stamped_4120:", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
