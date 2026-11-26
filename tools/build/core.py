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
import shutil
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
        # -fno-sanitize=vptr: the prebuilt LLVM DSO creates polymorphic
        # objects / shared_ptr control blocks (JITLink BinaryStreamRef,
        # ORC DefinitionGenerator, SymbolStringPool) whose vtables are not
        # in the instrumented test binary, so the vptr check reports an
        # "invalid vptr" on a valid object. It fires inside LLVM's own
        # LLJITBuilder/create path, out of reach of a source workaround
        # (#5461 hit the same false positive in getSymbolStringPool).
        "-fsanitize=undefined -fno-sanitize=vptr -fno-omit-frame-pointer",
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


# Issue #5246: toolchain / job-count helpers shared by the command groups.
def _build_jobs() -> int:
    raw = os.environ.get("AURA_BUILD_JOBS", "").strip()
    if raw:
        try:
            n = int(raw)
            if n > 0:
                return n
        except ValueError:
            pass
    return os.cpu_count() or 4


def _tool_available(name: str) -> bool:
    return shutil.which(name) is not None


def _linker_binary_runs(name: str) -> bool:
    """True if `name` is on PATH and can start (not a broken .so link)."""
    path = shutil.which(name)
    if path is None:
        return False
    try:
        r = subprocess.run([path, "--version"], capture_output=True, timeout=5)
        return r.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _env_flag(name: str, default: bool = False) -> bool:
    """Parse AURA_*/env truthy flags: 1/true/yes/on vs 0/false/no/off."""
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _cxx_driver() -> str:
    """Compiler driver used for link probes (matches CMake preference)."""
    for cand in (
        os.environ.get("CXX", "").strip(),
        "c++",
        "g++",
        "clang++",
    ):
        if cand and shutil.which(cand):
            return cand
    return "c++"


def _fuse_ld_works(fuse_ld: str) -> bool:
    """Probe whether ``-fuse-ld=<name>`` can link a trivial C++ program.

    GCC 16 ships ``libatomic_asneeded.so`` linker scripts that older mold
    mis-parses (``library not found: AS_NEEDED``), which breaks CMake's
    compiler identification. Prefer probing over hard-coding versions.
    """
    import tempfile

    cxx = _cxx_driver()
    try:
        with tempfile.TemporaryDirectory(prefix="aura_ldprobe_") as td:
            src = Path(td) / "t.cpp"
            out = Path(td) / "t.out"
            src.write_text("int main(){return 0;}\n", encoding="utf-8")
            r = subprocess.run(
                [cxx, f"-fuse-ld={fuse_ld}", str(src), "-o", str(out)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if r.returncode == 0 and out.is_file():
                return True
            err = (r.stderr or "") + (r.stdout or "")
            # Keep the first useful line for CI logs when we fall back.
            first = next((ln.strip() for ln in err.splitlines() if ln.strip()), "")
            if first:
                warn(f"linker probe -fuse-ld={fuse_ld} failed: {first[:160]}")
            return False
    except (OSError, subprocess.TimeoutExpired) as e:
        warn(f"linker probe -fuse-ld={fuse_ld} error: {e}")
        return False


def _select_fast_linker() -> str | None:
    """Pick mold / lld when available *and* able to link with the toolchain.

    Returns the ``-fuse-ld=`` value (``mold`` / ``lld``) or None for default ld.
    """
    # AURA_USE_MOLD=0 disables both mold and lld fast-path (classic ld.bfd).
    if not _env_flag("AURA_USE_MOLD", default=True):
        return None
    # Prefer mold when it works; fall back to lld on GCC16/libatomic_asneeded
    # incompatibility (mold issue #1545 / Gentoo #968893).
    if _tool_available("mold") and _linker_binary_runs("mold") and _fuse_ld_works("mold"):
        return "mold"
    if _tool_available("ld.lld") and _linker_binary_runs("ld.lld") and _fuse_ld_works("lld"):
        return "lld"
    if _tool_available("mold"):
        warn("mold present but cannot link with this toolchain; falling back")
    return None


# Issue #5246: git-diff and ninja helpers shared by the gate/command groups.


def _git_changed_files(base: str = "origin/main") -> list[str]:
    """Changed paths vs base (merge-base...HEAD) + staged + unstaged + untracked."""
    files: set[str] = set()
    try:
        mb = subprocess.run(
            ["git", "merge-base", base, "HEAD"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if mb.returncode == 0 and mb.stdout.strip():
            r = subprocess.run(
                ["git", "diff", "--name-only", f"{mb.stdout.strip()}...HEAD"],
                cwd=ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
            if r.returncode == 0:
                files.update(ln.strip() for ln in r.stdout.splitlines() if ln.strip())
        for args in (
            ["git", "diff", "--name-only", "--cached"],
            ["git", "diff", "--name-only"],
            ["git", "ls-files", "--others", "--exclude-standard"],
        ):
            r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, check=False)
            if r.returncode == 0:
                files.update(ln.strip() for ln in r.stdout.splitlines() if ln.strip())
    except FileNotFoundError:
        pass
    return sorted(files)


def _ninja_build(targets: list[str], *, jobs: int, keep_going: bool = False) -> int:
    cmd = ["ninja", "-C", str(BUILD), f"-j{jobs}"]
    if keep_going:
        cmd.append("-k0")
    cmd.extend(targets)
    return run(cmd, cwd=ROOT)


def _ninja_phony_inputs(target: str) -> list[str]:
    """Members of a phony aggregate (e.g. all_test_issue_targets)."""
    try:
        proc = subprocess.run(
            ["ninja", "-C", str(BUILD), "-t", "query", target],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if proc.returncode != 0:
        return []
    names: list[str] = []
    in_inputs = False
    for ln in (proc.stdout or "").splitlines():
        if ln.strip().startswith("input:"):
            in_inputs = True
            continue
        if in_inputs:
            if ln.startswith("  ") and not ln.startswith("    ") and ln.strip().endswith(":"):
                break
            s = ln.strip()
            if s and s != "phony":
                names.append(s)
    return names
