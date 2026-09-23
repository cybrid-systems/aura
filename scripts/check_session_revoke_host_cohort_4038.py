#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4038: hard-fiber fiber_id=0 session revoke skips host grants;
# provenance_ok then bricks the tenant (residual of #3799).
#
# The fiberless outermost MutationBoundaryGuard (host/REPL/compiler thread,
# g_current_fiber==nullptr) exits with fiber_id=0. #3799's fail-closed arm
# refused the whole sweep: rows minted by that same host face
# (grant_fiber_id==0) stayed live with bound_mutation_id pointing at the
# exited mid. provenance_ok_locked's production mid join fails the WHOLE
# check on the first mismatching contributing grant, and effects_for_locked
# still ORs the stale row's bits — one leaked Mutate/MSE row denies every
# later check whose mid differs, and the epoch-0 row never cycles the
# K=64 retain window (#3844 keeps epoch honest at 0), so the tenant is
# bricked until something revokes the row by name.
#
# AC1 — revoke_session_grants_for_mid_locked: under fiber_id==0 +
#       production + hard_fiber_isolation the sweep revokes live session
#       rows with bound_mutation_id==mid AND grant_fiber_id==0 (host
#       cohort — the same key the mint used); grant_fiber_id!=0 rows are
#       orphan-bumped only (#3799 peer behavior preserved). No bare
#       revoke-nothing return remains in that arm.
# AC2 — provenance_ok_locked (production arm): a session_bound row whose
#       bound_mutation_id != prov.mutation_id is skipped (not a
#       contributor) instead of failing the whole check; bound_mutation_id==0
#       under production keeps the #3333/#3090 mid-join-zero deny.
# AC3 — epoch retain fence (hard face): once grant_min_valid_epoch > 0,
#       grant_epoch==0 fails the fence exactly like grant_epoch < min_valid
#       (an epoch-0 row can never cycle the window); gated to
#       production+hard_fiber so Soft/Off keep the epoch-0 skip. No epoch-1
#       invention on grant rows (#3844 stands).
# AC4 — effects_for_locked: with a join mid under the production face, a
#       stale session_bound row contributes no bits (no OR, no wildcard
#       bookkeeping); check_and_record_effect passes prov.mutation_id as
#       the join mid so require_effect composes with the provenance skip.
# AC5 — no second model: no new query key or revoke policy — the fix is
#       inside the existing revoke/provenance/effects trio; the #4038 stamp
#       lives next to the #3799 arm; Soft/Off mid-only revoke (#3241/#3799)
#       arms are untouched.
# AC6 — ACs extend tests/core/test_capability_single_use_consume.cpp
#       (runner run_test_inert_session_mid_3723, per #81934); no
#       test_issue_4038.cpp; no docs/design/4038-* (per #1655).
#
# Self-test:
#   python3 scripts/check_session_revoke_host_cohort_4038.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    """Remove // line comments and /* block comments */ so substring
    search does not false-positive on prose. Cheap state machine; good
    enough for source-cite checks (does not need to handle raw strings /
    trigraphs).
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []
    cap_raw = _read("src/core/capability_model.hh")
    # Whitespace-normalized, comment-stripped view for code-shape checks:
    # comments must not satisfy behavior cites, and clang-format may wrap
    # signatures/predicates across lines.
    cap = " ".join(_strip_cpp_comments(cap_raw).split())
    test_src = _read("tests/core/test_capability_single_use_consume.cpp")
    build_src = _read("build.py")

    def must(cond: bool, label: str) -> None:
        if not cond:
            fails.append(label)

    # ── AC1: host-cohort revoke carve-out inside the #3799 fail-closed arm.
    must("Issue #4038" in cap_raw, "AC1: registry cites Issue #4038")
    must(
        "kCapabilitySessionRevokeHostCohortIssue = 4038" in cap,
        "AC1: #4038 stamp constant next to the revoke arm",
    )
    must(
        "if (g.grant_fiber_id != 0)" in cap and "std::size_t host_revoked = 0;" in cap,
        "AC1: peer rows (grant_fiber_id!=0) orphan-skipped; host rows revoke",
    )
    # The old arm ended in `return 0` after bumping orphans only; the fixed
    # arm must revoke host rows and return the revoked count.
    must(
        "return host_revoked;" in cap,
        "AC1: fail-closed arm returns the host revoke count (not a bare 0)",
    )

    # ── AC2: provenance stale-session skip in the production mid join.
    must(
        "g.session_bound && g.bound_mutation_id != prov.mutation_id" in cap,
        "AC2: stale session row skip predicate in provenance join",
    )
    must(
        "capability_mid_join_zero_deny_total" in cap,
        "AC2: mid-join-zero deny (#3333/#3090) preserved",
    )

    # ── AC3: epoch-0 retain fence under the hard face.
    must(
        "g.grant_epoch == 0" in cap,
        "AC3: epoch-0 retain fence present",
    )
    must(
        "capability_epoch_fence_hit_total" in cap,
        "AC3: epoch fence hit reuses the existing counter",
    )
    must(
        "stamp_grant_mutation_epoch" in cap and "kGrantEpochNoPhantomIssue = 3844" in cap,
        "AC3: #3844 honest-epoch stamp untouched (no epoch invention)",
    )

    # ── AC4: effects join filter + check_and_record_effect join mid.
    must(
        "effects_for_locked(TenantId tenant, std::uint64_t join_mid = 0)" in cap,
        "AC4: effects_for_locked takes optional join mid",
    )
    must(
        "effects_for_locked(tenant, prov.mutation_id)" in cap,
        "AC4: check_and_record_effect joins posture bits on prov mid",
    )

    # ── AC5: no second model — legacy arms and cites intact.
    must(
        "Issue #3799" in cap_raw and "Issue #3241" in cap_raw,
        "AC5: #3799/#3241 legacy arms still cited",
    )
    must(
        "revoke_session_grants_on_steal_or_abort_locked" in cap,
        "AC5: steal/abort locked sibling unchanged (no second revoke policy)",
    )

    # ── AC6: test binding + no invent.
    must("Issue #4038" in test_src, "AC6: test TU carries #4038 ACs")
    must("ac4038_1_" in test_src and "ac4038_5_" in test_src, "AC6: ac4038_* ACs present")
    must("run_test_inert_session_mid_3723" in test_src, "AC6: ACs wired into dispatched runner")
    must(not (ROOT / "tests/core/test_issue_4038.cpp").exists(), "AC6: no test_issue_4038.cpp")
    must(not (ROOT / "docs/design/4038-host-cohort-revoke.md").exists(), "AC6: no docs/design/")
    must(
        "check_session_revoke_host_cohort_4038" in build_src,
        "AC6: build.py wires the linter",
    )

    for f in fails:
        print(f"FAIL {f}", file=sys.stderr)
    if fails:
        print(f"check_session_revoke_host_cohort_4038: {len(fails)} failure(s)", file=sys.stderr)
        return 1
    print("check_session_revoke_host_cohort_4038: OK (6 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
