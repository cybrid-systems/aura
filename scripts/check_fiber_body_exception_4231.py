#!/usr/bin/env python3
"""Issue #4231: Soft --serve-async sock/holder dies mid set-code scoring
batch (serve_sock_death_mid_setcode_batch) — cold-restart thrash →
sock_transient (P1).

Hole: on Soft tip a9ddd94 (--serve-async, Soft Ready denseness #4047/
#4048, workers=1), after a host_parallel propose (64) the Soft unix sock
vanished and/or the holder PID changed mid set-code explorer scoring; the
host cold-restarted repeatedly (~14s attach each) and often finished
no_gain_cause=sock_transient (all explorers hits=0). Two holder-death
faces, both silent (no FATAL stderr line, holder PID changed, sock file
deleted):
  (a) a C++ exception escaping ANY fiber body crossed the ucontext resume
      boundary into the worker dispatch loop. swapcontext has no
      unwinding contract, so the escape landed in std::terminate and the
      whole holder died — under a 64-explorer batch on one shared Soft
      Evaluator one throwing denseness/scoring fiber was enough.
  (b) protocol status writes to a closed stdout reader (host restart
      thrash, timed-out reader) raised SIGPIPE — default disposition
      terminates the process.
Distinct from #4229 (sock alive, empty CASE display) and #4230
(sequential fiber:spawn+join register-after-notify lost wake). The
fiber:join defuse_version-mutated WARN is observability-only: the audited
joiner path re-checks and resumes; no teardown lives there.

Fix shape (contain at the two death faces; degrade to explicit error, no
process death):
  - Fiber::trampoline (src/serve/fiber.cpp) wraps the body in try/catch:
    std + non-standard arms both bump the process-wide contained-exception
    counter, emit a stderr notice, and still mark the fiber Done — the
    holder serves the remaining explorers. No rethrow into the worker
    dispatch loop.
  - run_serve_async + run_serve_async_bench (src/serve/serve_async.cpp)
    install ::signal(SIGPIPE, SIG_IGN) so a dead reader degrades to an
    EPIPE write failure on the existing explicit-error path while the
    holder stays up.

Contract (one row per AC):
  AC1  trampoline containment: the body call is wrapped in try/catch with
       BOTH arms (const std::exception& + ...), each arm bumps
       fiber_body_exception_total_, cites #4231, and there is NO rethrow
       into the worker loop; the contained fiber still reaches
       FiberState::Done
  AC2  counter surface: fiber.h declares the
       static std::atomic<std::uint64_t> fiber_body_exception_total_
       member and the [[nodiscard]] fiber_body_exception_total() noexcept
       accessor; fiber.cpp defines both (one definition site)
  AC3  SIGPIPE containment: run_serve_async AND run_serve_async_bench
       install ::signal(SIGPIPE, SIG_IGN) before scheduler work, each
       citing #4231 (holder-survival rationale)
  AC4  runtime ACs dispatched in tests/serve/test_concurrent.cpp
       (test_issue_4231_body_exception_contained,
       test_issue_4231_holder_survives_thrown_body,
       test_issue_4231_source_cite, test_issue_4231_sigpipe_containment);
       no tests/**/test_issue_4231.cpp; no docs/design/4231-*
  AC5  build.py wiring + root_check_allowlist.txt entry for this linter

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

    def absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: must not contain {n!r}")

    def squish(s: str) -> str:
        # clang-format may wrap a long run_test(...) call across lines;
        # match dispatch wiring on whitespace-normalized text.
        return " ".join(s.split())

    def window(src: str, begin: str, end: str, label: str, width: int = 5200) -> str:
        b = src.find(begin)
        if b == -1:
            fails.append(f"{label}: anchor {begin!r} missing")
            return ""
        e = src.find(end, b + 1)
        if e == -1:
            fails.append(f"{label}: end anchor {end!r} missing")
            return ""
        return src[b:e]

    # ── AC1: trampoline containment ────────────────────────────────
    fiber_src = _read("src/serve/fiber.cpp")
    if not fiber_src:
        fails.append("AC1: src/serve/fiber.cpp unreadable")
    tramp = window(
        fiber_src, "void Fiber::trampoline", "// ── Issue #1584: structured Fiber::join", "AC1: trampoline region"
    )
    if tramp:
        must("Issue #4231", "AC1: trampoline cites #4231", tramp)
        must("try {", "AC1: body wrapped in try", tramp)
        must("catch (const std::exception& e)", "AC1: std exception arm", tramp)
        must("catch (...)", "AC1: non-standard arm", tramp)
        must("fiber_body_exception_total_.fetch_add", "AC1: counter bumped", tramp)
        must("FiberState::Done", "AC1: contained fiber still reaches Done", tramp)
        absent("throw;", "AC1: no rethrow into the worker dispatch loop", tramp)
        # Both arms bump (two call sites inside the region).
        if tramp.count("fiber_body_exception_total_.fetch_add") < 2:
            fails.append("AC1: both catch arms must bump the counter")

    # ── AC2: counter surface ───────────────────────────────────────
    fiber_hdr = _read("src/serve/fiber.h")
    if not fiber_hdr:
        fails.append("AC2: src/serve/fiber.h unreadable")
    must(
        "static std::atomic<std::uint64_t> fiber_body_exception_total_;",
        "AC2: header declares the counter member",
        fiber_hdr,
    )
    must(
        "[[nodiscard]] static std::uint64_t fiber_body_exception_total() noexcept;",
        "AC2: header declares the accessor",
        fiber_hdr,
    )
    if fiber_src:
        if fiber_src.count("std::atomic<std::uint64_t> Fiber::fiber_body_exception_total_{0};") != 1:
            fails.append("AC2: exactly one counter definition in fiber.cpp")
        must(
            "std::uint64_t Fiber::fiber_body_exception_total() noexcept {",
            "AC2: accessor defined in fiber.cpp",
            fiber_src,
        )

    # ── AC3: SIGPIPE containment ───────────────────────────────────
    serve_src = _read("src/serve/serve_async.cpp")
    if not serve_src:
        fails.append("AC3: src/serve/serve_async.cpp unreadable")
    else:
        hits = serve_src.count("::signal(SIGPIPE, SIG_IGN)")
        if hits != 2:
            fails.append(f"AC3: run_serve_async + bench must both install SIG_IGN (found {hits}/2)")
        must("#include <csignal>", "AC3: csignal included", serve_src)
        run_async = window(
            serve_src,
            "void run_serve_async(int num_workers) {",
            "// 2. Create thread pool for blocking operations",
            "AC3: run_serve_async prologue",
        )
        must("Issue #4231", "AC3: run_serve_async cites #4231", run_async)
        must("::signal(SIGPIPE, SIG_IGN)", "AC3: run_serve_async installs SIG_IGN", run_async)
        bench = window(
            serve_src,
            "void run_serve_async_bench(const std::string& file_path",
            "// 2. Create scheduler with worker threads",
            "AC3: run_serve_async_bench prologue",
        )
        must("Issue #4231", "AC3: bench cites #4231", bench)
        must("::signal(SIGPIPE, SIG_IGN)", "AC3: bench installs SIG_IGN", bench)

    # ── AC4: runtime ACs dispatched; no new test-file / docs paths ──
    tc = _read("tests/serve/test_concurrent.cpp")
    if not tc:
        fails.append("AC4: tests/serve/test_concurrent.cpp unreadable")
    else:
        for fn in (
            "test_issue_4231_body_exception_contained",
            "test_issue_4231_holder_survives_thrown_body",
            "test_issue_4231_source_cite",
            "test_issue_4231_sigpipe_containment",
        ):
            must(f"static bool {fn}()", f"AC4: {fn} defined", tc)
            must(f'run_test("{fn}", {fn})', f"AC4: {fn} dispatched", squish(tc))
        must("#4231 AC1", "AC4: AC1 header present", tc)
        must("#4231 AC2", "AC4: AC2 header present", tc)
        must("#4231 AC3", "AC4: AC3 header present", tc)
        must("#4231 AC4", "AC4: AC4 header present", tc)
    for bad in ROOT.glob("tests/**/test_issue_4231.cpp"):
        fails.append(f"AC4: no tests/**/test_issue_4231.cpp allowed (found {bad})")
    for bad in ROOT.glob("docs/design/4231-*"):
        fails.append(f"AC4: no docs/design/4231-* allowed (found {bad})")

    # ── AC5: wiring ────────────────────────────────────────────────
    build_py = _read("build.py")
    must(
        'fbb4231_script = ROOT / "scripts" / "check_fiber_body_exception_4231.py"',
        "AC5: build.py wires this linter",
        build_py,
    )
    allowlist = _read("scripts/coverage/root_check_allowlist.txt")
    must("check_fiber_body_exception_4231.py", "AC5: allowlist entry", allowlist)

    if fails:
        print(f"FAIL check_fiber_body_exception_4231 ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("ok check_fiber_body_exception_4231 — all 5 AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
