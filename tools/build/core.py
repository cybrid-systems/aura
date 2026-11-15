"""Shared state for the Aura build CLI (#5246).

``build.py`` used to be one 27k-line script.  The dead per-linter wrappers
and the duplicate `cmd_lint` chain are gone; this package is where the
remaining command groups live.  This module is the bottom of the import
graph: paths, terminal colours, the process runner, and the `--sanitizer`
routing that every command group shares.  It must not import `build.py` or
any command module.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

# Issue #1932: harness lives under tests/python/; bench cases under tests/bench/.
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "python"))
sys.path.insert(0, str(ROOT / "tests" / "bench"))
from _aura_harness import (  # noqa: E402
    B,
    G,
    N,
    R,
    Y,
    fail,
    info,
    ok,
    run,
    warn,
)

__all__ = [
    "AUDIT",
    "AURA",
    "B",
    "BENCH",
    "BUILD",
    "COVERAGE_CHECKS",
    "COVERAGE_MANIFESTS",
    "COVERAGE_RUN_CHECKS",
    "COVERAGE_RUNNER",
    "G",
    "N",
    "R",
    "ROOT",
    "SANITIZER_FLAGS",
    "SCRIPTS",
    "TEST_BIN",
    "TOOLS",
    "Y",
    "apply_sanitizer",
    "coverage_run",
    "fail",
    "info",
    "ok",
    "run",
    "warn",
]

SCRIPTS = ROOT / "scripts"
COVERAGE_CHECKS = SCRIPTS / "coverage" / "checks"
COVERAGE_RUNNER = SCRIPTS / "coverage" / "runner.py"
COVERAGE_RUN_CHECKS = SCRIPTS / "coverage" / "run_checks.py"
COVERAGE_MANIFESTS = SCRIPTS / "coverage" / "manifests"
TOOLS = SCRIPTS / "tools"
AUDIT = SCRIPTS / "audit"
BENCH = ROOT / "tests" / "benchmark.py"  # thin entry → tests/bench/benchmark.py


def _default_build_dir() -> Path:
    """Resolve build directory (Issue #1573: AURA_BUILD_DIR for macOS build-mac etc.)."""
    raw = os.environ.get("AURA_BUILD_DIR", "").strip()
    if raw:
        p = Path(raw)
        return p if p.is_absolute() else ROOT / p
    return ROOT / "build"


BUILD = _default_build_dir()
AURA = BUILD / "aura"
TEST_BIN = BUILD / "test_ir"

# ═══════════════════════════════════════════════════════════════
# Sanitizer configuration (Issue #299)
#
# Pass --sanitizer={asan|ubsan|tsan} to any build.py subcommand
# to route compilation into build_<san>/ with the right flags.
# No flag = normal Debug/RelWithDebInfo build, behavior unchanged.
# ═══════════════════════════════════════════════════════════════

# Each entry: (CFLAGS/CXXFLAGS, LDFLAGS, CMAKE_BUILD_TYPE override or None)
# -O1 is required for tsan (lower opt levels reduce false positives).
# frame-pointer is needed for clean stack traces under asan/ubsan.
SANITIZER_FLAGS = {
    "asan": (
        "-fsanitize=address -fno-omit-frame-pointer",
        "-fsanitize=address",
        None,  # honor user AURA_BUILD_TYPE
    ),
    "ubsan": (
        "-fsanitize=undefined -fno-omit-frame-pointer",
        "-fsanitize=undefined",
        None,
    ),
    "tsan": (
        # -Wno-error=mismatched-new-delete: GCC's TSan instrumentation inlines
        # LLVM IRBuilder CreateLoad (llvm/IR/IRBuilder.h:1950) and flags LLVM's
        # intrusive User::operator new/delete pairing as mismatched-new-delete;
        # with -Werror that kills the build (observed in aura_jit.cpp via
        # LLVMBuilder::load). Downgrade that one diagnostic to a warning —
        # all other -Werror checks stay on.
        # -Wno-error=tsan: GCC 16 flags the atomic_thread_fence wrapper in
        # src/core/atomic_fence_port.h as a TSan-instrumentation warning
        # ("atomic_thread_fence ... not supported") and -Werror kills the
        # build (#4711). Same narrowing as the mismatched-new-delete entry.
        "-fsanitize=thread -fno-omit-frame-pointer -Wno-error=mismatched-new-delete -Wno-error=tsan",
        "-fsanitize=thread",
        "Debug",  # force -O0; -O2/-O3 explode TSan false positives
    ),
}


def apply_sanitizer(name: str) -> None:
    """Rebind BUILD/AURA/TEST_BIN to a sanitizer-specific build dir.

    Called from main() after parsing --sanitizer=NAME from sys.argv.
    Idempotent: empty name restores the default build/ tree (or AURA_BUILD_DIR).
    """
    global BUILD, AURA, TEST_BIN
    if name:
        if name not in SANITIZER_FLAGS:
            fail(f"unknown --sanitizer={name!r} (choose from: asan, ubsan, tsan)")
            sys.exit(2)
        BUILD = ROOT / f"build_{name}"
    else:
        BUILD = _default_build_dir()
    AURA = BUILD / "aura"
    TEST_BIN = BUILD / "test_ir"


def _issue_from_check_stem(stem: str) -> str | None:
    for part in reversed(stem.replace("-", "_").split("_")):
        if part.isdigit() and 3 <= len(part) <= 5:
            return part
    return None


def _manifest_issue_for_script(filename: str) -> str | None:
    """A folded check with no issue digits is named by exactly one manifest."""
    needle = f"coverage/checks/{filename}"
    hits = [
        path.stem
        for path in COVERAGE_MANIFESTS.glob("*.json")
        if needle in path.read_text(encoding="utf-8", errors="replace")
    ]
    if len(hits) == 1 and hits[0].isdigit():
        return hits[0]
    return None


def coverage_run(script: Path) -> subprocess.CompletedProcess:
    """Run a check_*.py, or its folded manifest if the script was removed."""
    if script.exists():
        return subprocess.run([sys.executable, str(script)], cwd=ROOT)
    issue = _issue_from_check_stem(script.stem) or _manifest_issue_for_script(script.name)
    if issue and (COVERAGE_MANIFESTS / f"{issue}.json").is_file():
        return subprocess.run([sys.executable, str(COVERAGE_RUNNER), "--issue", issue], cwd=ROOT)
    fail(f"missing {script}")
    return subprocess.CompletedProcess(args=[], returncode=1)
