#!/usr/bin/env python3
"""Issue #4131 source-cite gate: oneshot SIGTERM disposition reset.

Soft oneshot (`aura <file.aura>`, not --serve) survived the aura-build Soft
LeetCode verify timeout SIGTERM (palindrome-linked-list / ugly-number) and
only died under `os.killpg(SIGKILL)`. The aura binary never touched SIGTERM
(`git log -S SIGTERM -- src/` is empty — no handler ever existed), and POSIX
carries an ancestor's SIG_IGN for TERM across execve. So any harness
ancestor that ignored SIGTERM (daemonized holder, nohup-style batch runner)
leaves the exec'd oneshot TERM-immune: `terminate()` is a no-op, the spin
keeps running with SigIgn 0x4000 in /proc/<pid>/status, and only SIGKILL
reclaims it. Reproduced at HEAD: a fork-child with pre-exec
signal(SIGTERM, SIG_IGN) exec'ing build/aura on a `(while #t ...)` spin
survives terminate(); with the oneshot SIG_DFL reset it dies by SIGTERM
within the bounded wait. The fix shape: the oneshot fallthrough in main.cpp
resets TERM to SIG_DFL (serve branches return earlier — untouched).

ACs:
  AC1  src/main.cpp's oneshot fallthrough resets the TERM disposition with
       ::signal(SIGTERM, SIG_DFL), positioned AFTER the
       "Normal REPL / pipe / file / -e mode" banner (serve faces return
       from earlier branches — untouched).
  AC2  the reset site cites #4131 in its rationale comment.
  AC3  src/ contains exactly ONE SIGTERM site — the SIG_DFL reset; no
       sigaction(SIGTERM) handler anywhere (prompt default-death semantics,
       not graceful-flag).
  AC4  tests/compiler/test_fiber_spawn_cli.cpp hosts the runtime doors
       (#4131 AC9..AC12 + spawn/TERM helpers); no tests/**/test_issue_4131.cpp
       (per #81934); no docs/design/4131-* (per #1655).
  AC5  build.py wires scripts/check_oneshot_sigterm_4131.py and
       scripts/coverage/root_check_allowlist.txt lists it.

Exit 0 = all rows satisfied. Exit 1 = missing gate or contract row.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "src" / "main.cpp"
TST = ROOT / "tests" / "compiler" / "test_fiber_spawn_cli.cpp"


def main() -> int:
    main_src = MAIN.read_text() if MAIN.exists() else ""
    tst = TST.read_text() if TST.exists() else ""
    build = (ROOT / "build.py").read_text()
    allow = (ROOT / "scripts" / "coverage" / "root_check_allowlist.txt").read_text()
    ok = True

    def report(name: str, good: bool, msg: str) -> None:
        nonlocal ok
        print(f"  {'PASS' if good else 'FAIL'}: {name}: {msg}")
        ok = ok and good

    banner = main_src.find("Normal REPL / pipe / file / -e mode")
    reset = main_src.find("::signal(SIGTERM, SIG_DFL)")
    good = banner >= 0 and reset > banner
    report(
        "AC1",
        good,
        "oneshot fallthrough resets TERM to SIG_DFL after the REPL/pipe/file/-e "
        "banner (serve branches already returned)",
    )

    cite = main_src.find("#4131")
    good = cite >= 0 and reset >= 0 and 0 < abs(cite - reset) < 1600
    report("AC2", good, "reset site cites #4131 in its rationale comment")

    sig_rows: list[str] = []
    for pat in ("*.cpp", "*.h", "*.hpp", "*.ixx"):
        for p in (ROOT / "src").rglob(pat):
            try:
                for i, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
                    if "SIGTERM" in line and not line.strip().startswith("//"):
                        sig_rows.append(f"{p.relative_to(ROOT)}:{i}: {line.strip()}")
            except OSError:
                pass
    good = len(sig_rows) == 1 and "::signal(SIGTERM, SIG_DFL)" in sig_rows[0] and "::sigaction(SIGTERM" not in main_src
    report(
        "AC3",
        good,
        f"exactly one SIGTERM code site in src/ — the oneshot SIG_DFL reset; no handler (found {len(sig_rows)} rows)",
    )

    no_doc = not any((ROOT / "docs" / "design").glob("4131-*")) if (ROOT / "docs" / "design").exists() else True
    issue_file = list((ROOT / "tests").rglob("test_issue_4131.cpp"))
    good = (
        "#4131 AC9" in tst
        and "#4131 AC10" in tst
        and "#4131 AC11" in tst
        and "#4131 AC12" in tst
        and "spawn_4131_oneshot" in tst
        and "term_4131_and_reap" in tst
        and not issue_file
        and no_doc
    )
    report(
        "AC4",
        good,
        "runtime ACs hosted in test_fiber_spawn_cli.cpp; no test_issue_4131.cpp; no docs/design/4131-*",
    )

    good = "check_oneshot_sigterm_4131.py" in build and "check_oneshot_sigterm_4131.py" in allow
    report("AC5", good, "build.py wires the linter; scripts/coverage/root_check_allowlist.txt lists it")

    print(f"check_oneshot_sigterm_4131: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
