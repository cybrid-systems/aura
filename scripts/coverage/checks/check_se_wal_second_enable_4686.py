#!/usr/bin/env python3
"""Issue #4686: second SecurityEvent WAL enable failure must fail closed.

Before the fix SecurityEventWal::enable() bumped
security_event_crash_recovery_success as soon as a non-empty replay was
collected, i.e. BEFORE open_segment_unlocked(). A failed second enable
(read-only existing segment) therefore left the WAL off but the counter
moved; Evaluator::enable_mutation_audit_wal read that delta as sidecar
replay success and returned true, so the force_wal fail-closed hold stayed
disarmed and EffectDeny only reached the 1024-entry ring.

Contract rows (control flow; the runtime rows live in the manifest +
tests/compiler/test_security_event_wal_replay.cpp):
  AC1 recovery stamp sits AFTER the open_segment_unlocked failure guard
  AC2 replay collection arm does not stamp recovery itself
  AC3 enable_mutation_audit_wal consumes the sidecar enable result and
      re-arms fail-closed on failure instead of trusting the counter
  AC4 no phantom "(void)enable_security_event_wal" swallow
  AC5 runtime test rows present; no tests/issues or docs/design file

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _window(src: str, start: str, end: str) -> str:
    pos = src.find(start)
    if pos < 0:
        return ""
    tail = src.find(end, pos + 1)
    if tail < 0:
        return src[pos:]
    return src[pos:tail]


def main() -> int:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    sew = _read("src/core/security_event_wal.hh")
    evs = _read("src/compiler/evaluator_security.cpp")
    test = _read("tests/compiler/test_audit_durable_gap_force_wal.cpp")

    enable_body = _window(sew, "bool enable(std::string_view persist_dir", "void disable() noexcept")
    pair_body = _window(
        evs, "bool Evaluator::enable_mutation_audit_wal(", "void Evaluator::disable_mutation_audit_wal("
    )

    if not enable_body:
        fails.append("AC1: SecurityEventWal::enable not found")
    if not pair_body:
        fails.append("AC3: enable_mutation_audit_wal not found")

    # AC1: exactly one recovery stamp, and it must follow the open guard.
    stamp = "security_event_crash_recovery_success.fetch_add("
    open_guard = "if (!open_segment_unlocked(any ? max_seg : 0))"
    must(stamp, "AC1", enable_body)
    must(open_guard, "AC1", enable_body)
    if enable_body:
        n_stamp = enable_body.count(stamp)
        if n_stamp != 1:
            fails.append(f"AC1: expected exactly 1 recovery stamp in enable(), found {n_stamp}")
        ipos = enable_body.find(open_guard)
        spos = enable_body.find(stamp)
        if not (ipos >= 0 and spos >= 0 and ipos < spos):
            fails.append("AC1: recovery stamp must come after the open_segment_unlocked failure guard")
        open_ok = enable_body.find("return false;", ipos)
        if not (spos > open_ok >= 0):
            fails.append("AC1: recovery stamp must come after the failed-open return")

    # AC2: the replay-collection arm only records, never stamps.
    replay_arm = _window(enable_body, "if (out_replay) {", "if (!open_segment_unlocked(")
    if not replay_arm:
        fails.append("AC2: replay arm not found")
    else:
        must("replayed_nonempty = !out_replay->empty();", "AC2", replay_arm)
        if stamp in replay_arm:
            fails.append("AC2: replay arm must not stamp crash recovery before the open")

    # AC3: consume the sidecar result, re-arm fail-closed, never trust the delta.
    must("Issue #4686", "AC3", pair_body)
    must("const bool se_ok = enable_security_event_wal(persist_dir);", "AC3", pair_body)
    must("if (!se_ok) {", "AC3", pair_body)
    must("wal_fail_closed_defaulted_by_force_wal() != 0", "AC3", pair_body)
    must("arm_force_wal_enable_fail();", "AC3", pair_body)
    if pair_body:
        se_ok_pos = pair_body.find("const bool se_ok = enable_security_event_wal(persist_dir);")
        guard_pos = pair_body.find("if (!se_ok) {")
        arm_pos = pair_body.find("arm_force_wal_enable_fail();")
        delta_pos = pair_body.find("se_recov_after > se_recov_before")
        if not (se_ok_pos >= 0 and guard_pos > se_ok_pos):
            fails.append("AC3: se_ok guard must follow the sidecar enable")
        if not (arm_pos > guard_pos >= 0):
            fails.append("AC3: arm_force_wal_enable_fail must run inside the !se_ok arm")
        if not (delta_pos > guard_pos):
            fails.append("AC3: counter delta must not be consulted before the !se_ok arm")

    # AC4: no swallowed enable result anywhere.
    if "(void)enable_security_event_wal(" in evs:
        fails.append("AC4: (void)enable_security_event_wal still swallows the result")
    if "(void)enable_security_event_wal(persist_dir);" in evs:
        fails.append("AC4: phantom-success call shape still present")

    # AC5: runtime rows + no invented locations.
    for needle in (
        "4686 AC1: second enable reports failure",
        "4686 AC1: recovery counter not bumped by the failed open",
        "4686 AC1: enable-fail posture re-armed (was disarmed)",
        "4686 AC2: overflow reason is se-wal-enable-miss",
        "4686 AC3: non-empty replay now counts recovery success once",
        "4686 AC4: Soft failure does not arm the fail-closed posture",
    ):
        must(needle, "AC5", test)
    for rel in ("tests/issues/test_issue_4686.cpp", "tests/compiler/test_issue_4686.cpp"):
        if (ROOT / rel).is_file():
            fails.append(f"AC5: {rel} present (suite-aligned test only)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("4686-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #4686 second SE WAL enable failure re-arms fail-closed and stops stamping recovery early")
    return 0


if __name__ == "__main__":
    sys.exit(main())
