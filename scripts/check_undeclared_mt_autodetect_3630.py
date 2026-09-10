#!/usr/bin/env python3
# scripts/check_undeclared_mt_autodetect_3630.py -- Issue #3630 source-cite gate.
#
# AC1: detection at the per-Evaluator principal authority —
#      set_tenant_principal consults g_last_seen_nonzero_principal
#      (exchange, acq_rel); a SECOND distinct non-zero principal under
#      Restricted/Strict (sandbox gate, Off never records) bumps the
#      counter. Atoms + opt-out env (AURA_MT_AUTODETECT) + test reset in
#      provenance_tracker.hh (kUndeclaredMtAutodetectIssue = 3630).
# AC2: arm order capture -> fiber -> env flag (set_hard_capture_tenant ->
#      set_hard_fiber_isolation -> set_multi_tenant_env_active),
#      idempotent + monotonic; quota map follows the flag
#      (multi_tenant_env_active consult) with an arm-time cache refresh.
# AC3: undeclared_multi_tenant_detected_total appended at
#      TenantIsolationMetrics END (#2906) + snapshot struct + snapshot fn.
# AC4: Soft/Off zero-cost — the Off gate row in the detection condition;
#      AURA_MT_AUTODETECT opt-out keeps legacy behavior dark but
#      observable.
# AC5: one posture SE per process (once-guard exchange), stable reason
#      "undeclared-multi-tenant-armed", denied=false, new appended
#      SecurityEventKind PostureObserve = 6 (never renumbered).
# AC6: posture surface additive — both query:security-posture
#      registrations (slim obs_eval + full security last-wins) expose
#      undeclared-multi-tenant-detected-total + undeclared-mt-autodetect-
#      armed; no new query key.
# AC7: suite rows (ac3630 in test_require_effect_auto_isolation.cpp), no
#      tests/issues / docs/design, build.py registration.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SEC = "src/compiler/evaluator_security.cpp"
PROV = "src/core/provenance_tracker.hh"
ISO = "src/core/workspace_isolation.hh"
QUOTA = "src/core/resource_quota.hh"
SE = "src/core/security_event.hh"
OBS = "src/compiler/evaluator_primitives_obs_eval.cpp"
SECP = "src/compiler/evaluator_primitives_security.cpp"
TEST = "tests/compiler/test_require_effect_auto_isolation.cpp"
BUILD = "build.py"

LINTER = "check_undeclared_mt_autodetect_3630"
REASON = "undeclared-multi-tenant-armed"
CITE = "#3630"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(
    sec: str,
    prov: str,
    iso: str,
    quota: str,
    se: str,
    obs: str,
    secp: str,
    test: str,
    build: str,
) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — detection at the principal authority.
    must("Issue #3630", "AC1 detection cite", sec)
    must("g_last_seen_nonzero_principal().exchange", "AC1 exchange consult", sec)
    must("prev != 0 && prev != tenant_id", "AC1 second-distinct gate", sec)
    must(
        "(sandbox_mode_ != 0 || effect_sandbox_mode() != 0)",
        "AC1 Off gate (Soft/Off never records)",
        sec,
    )
    must("kUndeclaredMtAutodetectIssue = 3630", "AC1 header stamp", prov)
    must("g_last_seen_nonzero_principal", "AC1 last-seen atom", prov)
    must("AURA_MT_AUTODETECT", "AC1 opt-out env", prov)
    must("mt_autodetect_disabled", "AC1 opt-out consult", sec)
    must("reset_undeclared_mt_autodetect_for_test", "AC1 test reset", prov)

    # AC2 — arm order capture -> fiber -> env flag; quota follows.
    i_cap = sec.find("set_hard_capture_tenant(true);")
    i_fib = sec.find("set_hard_fiber_isolation(true);", i_cap if i_cap >= 0 else 0)
    i_env = sec.find("set_multi_tenant_env_active(true);", i_fib if i_fib >= 0 else 0)
    if not (0 <= i_cap < i_fib < i_env):
        fails.append("AC2: arm order must be capture -> fiber -> env flag")
    must(
        "multi_tenant_env_active();",
        "AC2 quota consults the flag",
        quota,
    )
    must("refresh_quota_per_tenant_cache", "AC2 quota cache refresh on arm", quota)

    # AC3 — counter appended at metrics END + snapshot pair.
    must("undeclared_multi_tenant_detected_total{0}", "AC3 metrics counter", iso)
    must("undeclared_multi_tenant_detected = 0", "AC3 snapshot struct field", iso)
    must(
        "m.undeclared_multi_tenant_detected_total.load(std::memory_order_relaxed)",
        "AC3 snapshot fn load",
        iso,
    )

    # AC4 — opt-out env parse lives next to the flag atoms.
    must('"AURA_MT_AUTODETECT"', "AC4 opt-out env literal", prov)

    # AC5 — one posture SE, stable reason, appended kind.
    must("g_undeclared_mt_se_emitted().exchange", "AC5 SE once-guard", sec)
    must(REASON, "AC5 stable reason", sec)
    must("/*denied=*/false", "AC5 observability-only", sec)
    must("PostureObserve = 6", "AC5 appended kind", se)

    # AC6 — posture surface additive on both registrations.
    for hay, label in ((obs, "AC6 slim"), (secp, "AC6 full")):
        must("undeclared-multi-tenant-detected-total", f"{label} counter key", hay)
        must("undeclared-mt-autodetect-armed", f"{label} armed key", hay)
    must_not("query:undeclared", "AC6 no new query key", sec)
    must_not("query:undeclared", "AC6 no new query key", prov)

    # AC7 — suite + registration + no invented artifacts.
    must("ac3630", "AC7 suite rows", test)
    must("Issue #3630", "AC7 suite cite", test)
    must(LINTER, "AC7 build.py registration", build)
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3630.cpp"))
    if invented:
        fails.append(f"AC7: invented test_issue_3630.cpp present: {invented}")
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3630*"))
        if hits:
            fails.append(f"AC7: docs/design/*3630* present: {hits}")
    return fails


def _self_test() -> int:
    sec_ok = (
        "void Evaluator::set_tenant_principal(std::uint64_t tenant_id) noexcept {\n"
        "    capability_tenant_id_ = tenant_id;\n"
        "    allow_cross_tenant_ = false;\n"
        "    // Issue #3630: per-Evaluator principal authority detection.\n"
        "    if (tenant_id != 0 && !multi_tenant_env_active() &&\n"
        "        (sandbox_mode_ != 0 || effect_sandbox_mode() != 0)) {\n"
        "        const auto prev = g_last_seen_nonzero_principal().exchange(\n"
        "            tenant_id, std::memory_order_acq_rel);\n"
        "        if (prev != 0 && prev != tenant_id) {\n"
        "            g_tenant_isolation_metrics().undeclared_multi_tenant_detected_total"
        ".fetch_add(1);\n"
        "            if (g_undeclared_mt_se_emitted().exchange(1) == 0)\n"
        "                emit_security_event_durable(SecurityEventKind::PostureObserve,\n"
        '                                            tenant_id, 1, 0, 0, "set-tenant-principal",\n'
        '                                            "undeclared-multi-tenant-armed", /*denied=*/false, 0);\n'
        "            if (!mt_autodetect_disabled()) {\n"
        "                set_hard_capture_tenant(true);\n"
        "                g_capability_registry().set_hard_fiber_isolation(true);\n"
        "                set_multi_tenant_env_active(true);\n"
        "                refresh_quota_per_tenant_cache();\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}"
    )
    prov_ok = (
        "inline constexpr int kUndeclaredMtAutodetectIssue = 3630;\n"
        "inline std::atomic<std::uint64_t>& g_last_seen_nonzero_principal() noexcept { static std::atomic<std::uint64_t> v{0}; return v; }\n"
        '[[nodiscard]] inline bool mt_autodetect_disabled() noexcept { if (const char* e = std::getenv("AURA_MT_AUTODETECT"); e && *e) return true; return false; }\n'
        "inline void reset_undeclared_mt_autodetect_for_test() noexcept {}\n"
    )
    iso_ok = (
        "std::atomic<std::uint64_t> nodeid_only_entry_prevented_total{0};\n"
        "std::atomic<std::uint64_t> undeclared_multi_tenant_detected_total{0};\n"
        "std::uint64_t undeclared_multi_tenant_detected = 0;\n"
        "m.undeclared_multi_tenant_detected_total.load(std::memory_order_relaxed),\n"
    )
    quota_ok = (
        'const bool on = env_on("AURA_QUOTA_PER_TENANT") || env_on("AURA_MULTI_TENANT") ||\n'
        "                multi_tenant_env_active();\n"
        "inline void refresh_quota_per_tenant_cache() noexcept {}\n"
    )
    se_ok = "PostureObserve = 6,"
    obs_ok = 'insert_kv("undeclared-multi-tenant-detected-total", 0); insert_kv("undeclared-mt-autodetect-armed", 0);'
    secp_ok = obs_ok
    test_ok = "// Issue #3630\nstatic void ac3630_1_detection_arms_idempotent() {}"
    build_ok = 'ROOT / "scripts" / "check_undeclared_mt_autodetect_3630.py"'

    good = _rows(sec_ok, prov_ok, iso_ok, quota_ok, se_ok, obs_ok, secp_ok, test_ok, build_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1

    # Bad: strip the second-distinct gate — AC1 must trip.
    bad = _rows(
        sec_ok.replace("prev != 0 && prev != tenant_id", "prev != 0"),
        prov_ok,
        iso_ok,
        quota_ok,
        se_ok,
        obs_ok,
        secp_ok,
        test_ok,
        build_ok,
    )
    if not any("second-distinct gate" in r for r in bad):
        print("self-test: gate-stripped fixture did not trip AC1")
        return 1

    # Bad: opt-out env removed — AC1/AC4 must trip.
    bad = _rows(
        sec_ok,
        prov_ok.replace("AURA_MT_AUTODETECT", "AURA_MT_AUTODETECT_X"),
        iso_ok,
        quota_ok,
        se_ok,
        obs_ok,
        secp_ok,
        test_ok,
        build_ok,
    )
    if not any("opt-out" in r for r in bad):
        print("self-test: opt-out-stripped fixture did not trip")
        return 1

    print(f"ok {LINTER} self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3630 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(
        _read(SEC),
        _read(PROV),
        _read(ISO),
        _read(QUOTA),
        _read(SE),
        _read(OBS),
        _read(SECP),
        _read(TEST),
        _read(BUILD),
    )
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
