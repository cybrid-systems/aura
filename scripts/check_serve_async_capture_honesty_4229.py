#!/usr/bin/env python3
"""Issue #4229 source-cite gate: serve-async stdout capture honesty (sock_score_collapse).

Soft --serve-async (Ready denseness profile, auto workers=1) scored LeetCode
explorers via set-code + eval-current / run-cases. After a long host-parallel
MiniMax propose (32-64), session scoring returned hits=0 for every explorer
including the oneshot-green baseline, while the same baseline .aura oneshot
still scored hits>0. The host classified this as sock_score_collapse and fell
back to oneshot rescoring, burning ~200k+ MiniMax tokens per slug.

Root cause — silent capture degradation. capture_stdout_during captures the
exec's stdout via dup/pipe/dup2 so program output (CASE lines) rides the JSON
status object. When the setup failed (dup/pipe/dup2 under fd pressure — the
same sustained-load class as the #4176 per-fiber eventfd budget), the old
shape ran the exec ANYWAY with the LIVE stdout and returned an empty display.
Two failure faces:

  (a) status=ok with a silently empty display — the host scores hits=0 for
      every explorer while the same source oneshot prints CASE lines and
      scores >0 (the reported sock_score_collapse);
  (b) program output interleaved into the JSON protocol stream (the client's
      line decoder sees stray "99" fragments).

Fix shape — fail-loud capture. capture_stdout_during returns
std::optional<std::pair<EvalResult, std::string>>: on setup failure it
returns nullopt WITHOUT executing the exec, and both session exec loops
(named + default) emit an explicit transient error line
("serve stdout capture unavailable (transient)") so the client sees a real
status and can retry — never a silent ok with an empty display, never a
live-stdout exec. The successful path is unchanged (single exec run, captured
display on the status object, #4079/#4095 single-write contract untouched).

ACs:
  AC1  serve_async.cpp capture_stdout_during returns
       std::optional<std::pair<std::invoke_result_t<Fn>, std::string>> and
       both setup-failure arms (dup/pipe and dup2) return std::nullopt with
       an Issue #4229 rationale comment; the exec is NOT run on the failure
       path.
  AC2  The silent live-stdout fallback (return {std::forward<Fn>(fn)(), {}})
       is gone from capture_stdout_during.
  AC3  Both session exec loops (named-session nsid + default-session sid)
       emit the explicit transient error line
       "serve stdout capture unavailable (transient)" via status_line_error
       (exactly 2 sites).
  AC4  The exec body (R result = std::forward<Fn>(fn)();) appears exactly
       once — only on the successful capture path.
  AC5  tests/serve/test_concurrent.cpp cites #4229 (runtime/source-cite ACs:
       capture fail-loud contract); no tests/serve/test_issue_4229.cpp; no
       docs/design/4229-*; build.py wires this linter;
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVE = ROOT / "src" / "serve" / "serve_async.cpp"
TST = ROOT / "tests" / "serve" / "test_concurrent.cpp"

CAPTURE_RET = "std::optional<std::pair<std::invoke_result_t<Fn>, std::string>>"
SILENT_FALLBACK = "return {std::forward<Fn>(fn)(), {}};"
TRANSIENT = '"serve stdout capture unavailable (transient)"'
EXEC_ONCE = "R result = std::forward<Fn>(fn)();"


def _capture_region(text: str) -> str:
    """capture_stdout_during region (template start to status_line_ok)."""
    idx = text.find("auto capture_stdout_during(Fn&& fn)")
    if idx < 0:
        return ""
    end = text.find("std::string status_line_ok", idx)
    return text[idx:end] if end > 0 else text[idx : idx + 6000]


def main() -> int:
    serve = SERVE.read_text() if SERVE.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg_out: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg_out}")
        ok = ok and good

    cap = _capture_region(serve)

    # ── AC1: fail-loud capture shape ──
    report(
        "AC1 capture-fail-loud",
        CAPTURE_RET in cap and cap.count("return std::nullopt;") == 2 and "Issue #4229: capture setup failed" in cap,
        "capture_stdout_during returns optional; both setup-failure arms return "
        "nullopt without running the exec (#4229)",
    )

    # ── AC2: silent live-stdout fallback removed ──
    report(
        "AC2 silent-fallback-gone",
        SILENT_FALLBACK not in cap,
        "the old silent fallback (exec on live stdout + empty display) is removed (#4229)",
    )

    # ── AC3: both exec loops emit the transient error ──
    report(
        "AC3 transient-error-both-loops",
        serve.count(TRANSIENT) == 2,
        "named-session and default-session exec loops emit the explicit transient error line (2 sites) (#4229)",
    )

    # ── AC4: exec runs only on the successful capture path ──
    report(
        "AC4 exec-single-site",
        serve.count(EXEC_ONCE) == 1,
        "the exec body appears exactly once (successful capture path only) (#4229)",
    )

    # ── AC5: runtime door + wiring ──
    no_issue_file = not (ROOT / "tests" / "serve" / "test_issue_4229.cpp").exists()
    no_docs = not any((ROOT / "docs" / "design").glob("4229-*")) if (ROOT / "docs" / "design").exists() else True
    report(
        "AC5 test-door-and-wiring",
        "#4229" in tst
        and "test_issue_4229_capture_honesty" in tst
        and "check_serve_async_capture_honesty_4229.py" in build
        and "check_serve_async_capture_honesty_4229.py" in allow
        and no_issue_file
        and no_docs,
        "tests/serve/test_concurrent.cpp carries the #4229 ACs; build.py wires this "
        "linter; allowlist lists it; no test_issue_4229.cpp; no docs/design/4229-*",
    )

    if not ok:
        print("FAIL: check_serve_async_capture_honesty_4229")
        return 1
    print("PASS: check_serve_async_capture_honesty_4229 (5 ACs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
