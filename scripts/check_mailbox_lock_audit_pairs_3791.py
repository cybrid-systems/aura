#!/usr/bin/env python3
"""Issue #3791 — I5/I4 deploy-door fail-closed linter (test-only ship).

Residual of #3620 x #3763 x #3764: the PR soak stayed green under sticky
Mailbox TLS depth (on_acquire without on_release) and a no-edge
forever-held holder — the lock-order canary cannot observe either
condition, so the deploy door shipped false-green. This linter pins the
source-level doors next to the runtime asserts added to
tests/serve/test_mailbox_hold_starvation_hard.cpp:

  AC1: every mu_ acquisition in multi_fiber_mailbox.h carries an
       AuditScope(Level::Mailbox) immediately before it (acquire/release
       pairing — sticky-depth door).
  AC2: Guard-live recv/try_pop return Policy-A-empty BEFORE the first
       AuditScope / mu_ acquisition in their bodies (zero-mu_ door).
  AC3: no new query key and no g_3791_* counters (no mid-struct metrics).
  AC4: no new soak philosophy binary — the hard suite stays registered
       exactly once in CMake; no per-issue copy; no docs/design/3791*.
  AC5: lock_order depth machinery intact (on_release + g_depth +
       AuditScope).

Soft / Off is not a vulnerability: runtime hard doors skip under
!production_defaults_active(); this linter is mode-independent source
pinning.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAILBOX = ROOT / "src" / "serve" / "multi_fiber_mailbox.h"
LOCK_ORDER = ROOT / "src" / "compiler" / "lock_order_audit.h"
CMAKE = ROOT / "CMakeLists.txt"
QUERY_PRIMS = [
    ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp",
    ROOT / "src" / "compiler" / "evaluator_primitives_query_type_stats.cpp",
]
HARD_SUITE = "tests/serve/test_mailbox_hold_starvation_hard.cpp"
SRC_SUFFIXES = {".h", ".hpp", ".cpp", ".ixx"}
LOCK_NEEDLES = ("std::lock_guard lock(mu_)", "std::unique_lock<std::mutex> lock(mu_)")


def fail(msg: str) -> int:
    print(f"FAIL: {msg}")
    return 1


def check_mu_audit_pairing(text: str) -> list[str]:
    """AC1 — each mu_ acquisition has an AuditScope in its lock window."""
    errors: list[str] = []
    for needle in LOCK_NEEDLES:
        pos = 0
        while True:
            idx = text.find(needle, pos)
            if idx < 0:
                break
            window = text[max(0, idx - 700) : idx]
            if "AuditScope" not in window:
                line = text.count("\n", 0, idx) + 1
                errors.append(f"AC1: mu_ lock at line {line} has no AuditScope pairing")
            pos = idx + len(needle)
    return errors


def try_pop_ordering_errors(text: str) -> list[str]:
    """AC2 (try_pop) — Guard-skip must precede AuditScope/mu_."""
    tp = text.find("bool try_pop(MailMessage& out)")
    if tp < 0:
        return ["AC2: try_pop body not found"]
    win = text[tp : tp + 1600]
    skip = win.find("mutation_boundary_depth() > 0")
    scope = win.find("AuditScope")
    if skip < 0 or scope < 0 or skip > scope:
        return ["AC2: try_pop Guard-skip must precede AuditScope/mu_"]
    return []


def recv_ordering_errors(text: str) -> list[str]:
    """AC2 (recv) — this_fiber_holds (Policy A) must precede AuditScope/mu_."""
    rv = text.find("std::optional<MailMessage> recv(bool wait, int timeout_ms,")
    if rv < 0:
        return ["AC2: recv body not found"]
    win = text[rv : rv + 2600]
    holds = win.find("this_fiber_holds")
    scope = win.find("AuditScope")
    if holds < 0 or scope < 0 or holds > scope:
        return ["AC2: recv this_fiber_holds (Policy A) must precede AuditScope/mu_"]
    return []


def check_no_new_keys_or_counters() -> list[str]:
    """AC3 — no new query key, no g_3791_* mid-struct metrics."""
    errors: list[str] = []
    for q in QUERY_PRIMS:
        if q.exists() and "3791" in q.read_text(encoding="utf-8"):
            errors.append(f"AC3: {q.name} mentions 3791 (no new query key)")
    for p in (ROOT / "src").rglob("*"):
        if p.suffix not in SRC_SUFFIXES or not p.is_file():
            continue
        if "g_3791_" in p.read_text(encoding="utf-8", errors="ignore"):
            errors.append(f"AC3: {p.relative_to(ROOT)} adds g_3791_* counter")
    return errors


def check_no_new_soak_binary() -> list[str]:
    """AC4 — no new soak philosophy binary / per-issue copy / design doc."""
    errors: list[str] = []
    cmake = CMAKE.read_text(encoding="utf-8")
    if cmake.count(HARD_SUITE) != 1:
        errors.append("AC4: hard suite must stay registered exactly once in CMake")
    if (ROOT / "tests" / "serve" / "test_issue_3791.cpp").exists():
        errors.append("AC4: tests/serve/test_issue_3791.cpp must not exist")
    for p in (ROOT / "docs" / "design").glob("3791*"):
        errors.append(f"AC4: {p.name} must not exist")
    return errors


def check_lock_order_machinery(text: str) -> list[str]:
    """AC5 — depth pairing machinery still present."""
    errors: list[str] = []
    for needle in ("inline void on_release(Level L)", "g_depth[", "class AuditScope"):
        if needle not in text:
            errors.append(f"AC5: lock_order_audit.h missing {needle!r}")
    return errors


def self_test() -> int:
    unpaired = "void f() { std::lock_guard lock(mu_); }"
    if not check_mu_audit_pairing(unpaired):
        return fail("self-test: unpaired mu_ lock not detected")
    paired = "AuditScope s(Level::Mailbox);\n    std::lock_guard lock(mu_);"
    if check_mu_audit_pairing(paired):
        return fail("self-test: paired mu_ lock flagged")
    recv_bad = (
        "std::optional<MailMessage> recv(bool wait, int timeout_ms,\n"
        "                                std::uint64_t for_fiber, bool* stale) {\n"
        "    for (;;) {\n"
        "        { AuditScope rank(Level::Mailbox); std::lock_guard lock(mu_); }\n"
        "        const bool this_fiber_holds = depth() > 0;\n"
        "    }\n"
        "}"
    )
    if not recv_ordering_errors(recv_bad):
        return fail("self-test: mu_-before-Policy-A ordering not detected")
    recv_good = (
        "std::optional<MailMessage> recv(bool wait, int timeout_ms,\n"
        "                                std::uint64_t for_fiber, bool* stale) {\n"
        "    const bool this_fiber_holds = depth() > 0;\n"
        "    { AuditScope rank(Level::Mailbox); std::lock_guard lock(mu_); }\n"
        "}"
    )
    if recv_ordering_errors(recv_good):
        return fail("self-test: Policy-A-first recv flagged")
    print("self-test ok")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    if not MAILBOX.exists():
        return fail(f"missing {MAILBOX}")
    errors: list[str] = []
    mailbox = MAILBOX.read_text(encoding="utf-8")
    errors += check_mu_audit_pairing(mailbox)
    errors += try_pop_ordering_errors(mailbox)
    errors += recv_ordering_errors(mailbox)
    errors += check_no_new_keys_or_counters()
    errors += check_no_new_soak_binary()
    if LOCK_ORDER.exists():
        errors += check_lock_order_machinery(LOCK_ORDER.read_text(encoding="utf-8"))
    else:
        errors.append("AC5: missing src/compiler/lock_order_audit.h")
    for e in errors:
        print(f"FAIL: {e}")
    if errors:
        return 1
    print("check_mailbox_lock_audit_pairs_3791: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
