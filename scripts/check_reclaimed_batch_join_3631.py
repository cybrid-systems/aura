#!/usr/bin/env python3
# scripts/check_reclaimed_batch_join_3631.py -- Issue #3631 source-cite gate.
#
# AC1: join_agents defers the Reclaimed residual wait to ONE shared-budget
#      batch pass (maybe_auto_wait_reclaimed_batch) — the serial per-handle
#      auto-wait (maybe_auto_wait_reclaimed_production) is gone from the
#      join_agents body; wall ~= shared budget, not N x budget.
# AC2: batch helper contract — shared-deadline poll (NOT Fiber::join:
#      re-join races residual cleanup contracts, see wait_reclaimed_body);
#      Done path: note_body_exit_if_reclaimed + complete_agent_join_cleanup
#      (+ wait_reclaimed_cleanup_total); expiry: wait_reclaimed_timeout_
#      total per residual fiber, must_wait stays set (#3146), #3529/#3564
#      quota recycle per handle, host_forget_reclaimed_risk_total bumps
#      ONCE per batch.
# AC3: single-handle join_agent unchanged — still routes the SSOT wrapper
#      (maybe_auto_wait_reclaimed_production); wait_reclaimed_body poll
#      cadence + contract note intact.
# AC4: no new query key; no docs/design/3631*; no tests/issues/
#      test_issue_3631.cpp; build.py registration.
# AC5: suite rows (ac3631 in tests/orch/test_join_drain_reclaim.cpp).

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

AS = "src/orch/agent_spawn.h"
TEST = "tests/orch/test_join_drain_reclaim.cpp"
BUILD = "build.py"

LINTER = "check_reclaimed_batch_join_3631"
BATCH = "maybe_auto_wait_reclaimed_batch"
SERIAL = "maybe_auto_wait_reclaimed_production"
SIG = "join_agents(std::span<AgentHandle> agents,"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(asrc: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — join_agents defers to the batch pass; serial wrapper gone.
    must(BATCH, "AC1 batch helper present", asrc)
    must("Issue #3631", "AC1 cite", asrc)
    sig = asrc.find(SIG)
    if sig == -1:
        fails.append("AC1: join_agents signature not found")
        return fails
    ret = asrc.find("return jr;", sig)
    if ret == -1:
        fails.append("AC1: join_agents body end not found")
        return fails
    body = asrc[sig:ret]
    must(BATCH, "AC1 join_agents routes the batch pass", body)
    must_not(f"{SERIAL}(a,", "AC1 serial per-handle wrapper gone", body)

    # AC2 — batch helper contract (windowed on the helper definition).
    hdef = asrc.find(f"{BATCH}(std::span<AgentHandle> agents,")
    if hdef == -1:
        fails.append("AC2: batch helper definition not found")
        return fails
    # Back-extend: the helper's doc comment (contract note) sits above the
    # struct definition.
    hstart = max(0, hdef - 1200)
    helper = asrc[hstart : hdef + 4200]
    must(
        "std::this_thread::sleep_for(std::chrono::microseconds(200));",
        "AC2 shared-deadline poll cadence",
        helper,
    )
    must(
        "re-join would race residual cleanup contracts",
        "AC2 no-re-join contract cite",
        helper,
    )
    must("note_body_exit_if_reclaimed", "AC2 Done path body-exit note", helper)
    must("complete_agent_join_cleanup", "AC2 Done-path cleanup", helper)
    must(
        "wait_reclaimed_cleanup_total.fetch_add",
        "AC2 Done-path cleanup counter",
        helper,
    )
    must(
        "wait_reclaimed_timeout_total.fetch_add",
        "AC2 expiry timeout counter",
        helper,
    )
    must("maybe_force_release_reclaimed_quota", "AC2 quota recycle per handle", helper)
    must("#3146", "AC2 must_wait host-visible cite", helper)
    hf = helper.count("host_forget_reclaimed_risk_total.fetch_add")
    if hf != 1:
        fails.append(f"AC2: host_forget must bump exactly once per batch (found {hf})")

    # AC3 — single-handle join_agent unchanged (SSOT wrapper retained).
    must(
        f"{SERIAL}(h, /*caller_passed_wait_reclaimed_ms=*/false,",
        "AC3 join_agent SSOT wrapper retained",
        asrc,
    )
    wrb = asrc.find("wait_reclaimed_body(AgentHandle& h")
    if wrb == -1:
        fails.append("AC3: wait_reclaimed_body not found")
    else:
        wrb_win = asrc[wrb : wrb + 2600]
        must("microseconds(200)", "AC3 poll cadence intact", wrb_win)
        must("no Fiber::join", "AC3 no-re-join note intact", wrb_win)

    # AC4 — no invented artifacts; registration.
    must(LINTER, "AC4 build.py registration", build)
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3631.cpp"))
    if invented:
        fails.append(f"AC4: invented test_issue_3631.cpp present: {invented}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3631*"))
        if hits:
            fails.append(f"AC4: docs/design/*3631* present: {hits}")

    # AC5 — suite rows.
    must("ac3631", "AC5 suite rows", test)
    must("Issue #3631", "AC5 suite cite", test)
    return fails


def _self_test() -> int:
    asrc_ok = (
        "inline std::uint64_t maybe_auto_wait_reclaimed_production(\n"
        "    AgentHandle& h, bool caller_passed, std::uint64_t b) noexcept {\n"
        "    maybe_auto_wait_reclaimed_production(h, /*caller_passed_wait_reclaimed_ms=*/false, 1);\n"
        "}\n"
        "// Issue #3631\n"
        "struct WaitReclaimedBatchResult { std::uint64_t wait_us = 0; };\n"
        "inline WaitReclaimedBatchResult maybe_auto_wait_reclaimed_batch(std::span<AgentHandle> agents, std::uint64_t retry_budget_ms) noexcept {\n"
        "    std::this_thread::sleep_for(std::chrono::microseconds(200));\n"
        "    // re-join would race residual cleanup contracts\n"
        "    f->note_body_exit_if_reclaimed();\n"
        "    complete_agent_join_cleanup(a, done_jr);\n"
        "    g_orch_module_stats.wait_reclaimed_cleanup_total.fetch_add(1);\n"
        "    g_orch_module_stats.wait_reclaimed_timeout_total.fetch_add(1);\n"
        "    maybe_force_release_reclaimed_quota(a);\n"
        "    // #3146 host-visible signal\n"
        "    g_orch_module_stats.host_forget_reclaimed_risk_total.fetch_add(1);\n"
        "}\n"
        "join_agents(std::span<AgentHandle> agents, JoinPolicy policy) {\n"
        "    maybe_auto_wait_reclaimed_batch(agents, reclaimed_retry_budget_ms(policy.drain_ms));\n"
        "    return jr;\n"
        "}\n"
        "wait_reclaimed_body(AgentHandle& h, std::optional<std::uint64_t> timeout_ms) noexcept {\n"
        "    std::this_thread::sleep_for(std::chrono::microseconds(200));\n"
        "    // no Fiber::join — re-join would race residual cleanup contracts\n"
        "}"
    )
    test_ok = "// Issue #3631\nstatic void ac3631_1_batch_shared_budget() {}"
    build_ok = 'ROOT / "scripts" / "check_reclaimed_batch_join_3631.py"'

    good = _rows(asrc_ok, test_ok, build_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1

    # Bad 1: serial wrapper back inside join_agents — AC1 must trip.
    bad_asrc = asrc_ok.replace(
        "    maybe_auto_wait_reclaimed_batch(agents, reclaimed_retry_budget_ms(policy.drain_ms));\n",
        "    maybe_auto_wait_reclaimed_production(a, /*caller_passed_wait_reclaimed_ms=*/false, 1);\n",
    )
    bad = _rows(bad_asrc, test_ok, build_ok)
    if not any("serial per-handle wrapper gone" in r or "batch pass" in r for r in bad):
        print("self-test: serial-restore fixture did not trip AC1")
        return 1

    # Bad 2: host_forget bumped twice in the helper — AC2 must trip.
    bad_asrc2 = asrc_ok.replace(
        "    g_orch_module_stats.host_forget_reclaimed_risk_total.fetch_add(1);\n}",
        "    g_orch_module_stats.host_forget_reclaimed_risk_total.fetch_add(1);\n"
        "    g_orch_module_stats.host_forget_reclaimed_risk_total.fetch_add(1);\n"
        "}",
    )
    bad = _rows(bad_asrc2, test_ok, build_ok)
    if not any("exactly once per batch" in r for r in bad):
        print("self-test: double host_forget fixture did not trip AC2")
        return 1

    print(f"ok {LINTER} self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3631 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(AS), _read(TEST), _read(BUILD))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
