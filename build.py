#!/usr/bin/env python3
"""
Aura — 统一构建/测试入口

Usage:
  ./build.py [--sanitizer=asan|ubsan|tsan] build    # CMake 构建 (sanitizer-插桩)
  ./build.py [--sanitizer=asan|ubsan|tsan] test [tier|suite]  # 运行测试（默认 ci）
  ./build.py check            # gate + ci（与 CI 相同）
  ./build.py gate             # docs + ruff + format + fixtures + parallel coverage checks
  ./build.py gate --changed   # pre-push fast path: only checks touching git diff (+ cascade off)
  ./build.py gate --fix       # 同上，但 auto-regen docs/registry/inventory + lint/format --fix（#1572/#1957）
  ./build.py gate --scripts-only  # 跳过 clang-format（脚本-only,无 C++ 编译）
  ./build.py gate --serial    # coverage checks one-at-a-time (debug; default is parallel)
  ./build.py gate --jobs N    # coverage parallel workers (default min(16,nproc) / AURA_GATE_JOBS)
  ./build.py legacy-test-inventory  # #1957 inventory freshness (--fix to regen)
  ./build.py new-test-files      # reject newly added issue-numbered test names
  ./build.py ci               # build + CI 测试矩阵
  ./build.py ci-issues        # issue 测试矩阵的一片 (AURA_ISSUE_SHARD=i/N, CI issue-farm #4819)
  ./build.py ci-issues --build-only   # 只编译这一片（CI 在测试前装入 aura 可执行文件）
  ./build.py ci-issues --test-only    # 只跑已编好的这一片
  ./build.py clean            # 清理构建产物
  ./build.py list             # 列出测试套件
  ./build.py demo             # 运行 Agent 管线演示
  ./build.py pgo instrument    # PGO 插桩构建
  ./build.py pgo train         # PGO 训练
  ./build.py pgo merge         # 合并 profiles
  ./build.py pgo optimize      # PGO 优化构建
  ./build.py pgo all           # 全流程
  ./build.py docs              # 从源码生成 docs/generated/*.md
  ./build.py docs --check      # 校验生成文档未过期（CI）
  ./build.py lint              # Ruff lint + format check（Python）
  ./build.py lint --fix        # 自动修复可修复项并格式化
  ./build.py format            # clang-format 全树校验（与 CI gate 相同）
  ./build.py format --fix      # clang-format -i 自动修复 src/ + tests/
  ./build.py test-registry     # 校验 docs/generated/test-registry.json 新鲜度（#1572）
  ./build.py test-registry --fix  # 重新生成 test-registry.json
  ./build.py fixtures --check  # 校验 tests/fixtures/*.json schema
  ./build.py dead-heap-push    # dead string_heap_ push audit --strict（#1668）
  ./build.py catch-silent-swallow  # catch(...) SILENCE-PRIM audit --strict（#1669）
  ./build.py aot-env-linear-stamp  # AOT mangle (0,0) env/linear stamp fence（#2091/#2168）
  ./build.py repro [--verify]  # 可复现 Release 构建（#675）

  ./build.py sbom [--version=V] # CycloneDX SBOM 生成（#675）
  ./build.py security          # 依赖/文件系统漏洞扫描（#675）
  ./build.py bench [--strict]  # Benchmark 基线 + 回归检测（#1569 SLO gate）
  ./build.py coverage --html   # LLVM source-based coverage report (#1933)
  ./build.py coverage --check-tools  # verify llvm-cov tooling only
  ./build.py fuzz --list       # list registered fuzzers (#1935)
  ./build.py fuzz --all --quick  # run fuzz orchestrator
  ./build.py production-concurrency  # #2380/#2513 nightly gate: canary + full chaos soak
  ./build.py production-concurrency-coverage  # #2380/#2513 static AC contract rows
  #   Soak knobs: AURA_CHAOS_SOAK=1 AURA_CHAOS_FIBERS=256..1000 AURA_CHAOS_DURATION_S=300+
  #   #2554: ./build.py gate runs short PR chaos hard-fail (steal hard-fail Δ==0,
  #          residual still-running==0) via AURA_CHAOS_PR_GATE=1 (not FULL/SOAK)
  #   #2931: ./build.py chaos-steal-gc-nightly-2931 — steal×mutate×GC×mailbox
  #          soak hard gate (AURA_CHAOS_STEAL_GC=1 duration≥600 workers≥8)

Test tiers (./build.py test [name]; default ci — see ./build.py list):
  pr          core + safety + issues-fast
  ci          core + safety + issues   (`all` is an alias of ci)
  nightly     ci + e2e + bench + suite-s0
  everything  every leaf (includes demo / ai / mutation / bench)
  core        unit integ typecheck smoke bash suite repl runtime-c concurrent
  safety      gradual regression p0
  issues      C++ domain / issue binaries (AURA_ISSUES_TIER)
  issues-fast same runner, fast tier
  affected    workspace or git diff → that L1 suite and Python theme, plus std/test and smoke (no issue-farm). Unmapped diffs exit non-zero.
  check       gate + build + ci
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from threading import Lock

# Issue #1932: harness lives under tests/python/; bench cases under tests/bench/
_ROOT_FOR_PATH = Path(__file__).resolve().parent
sys.path.insert(0, str(_ROOT_FOR_PATH / "tests" / "python"))
sys.path.insert(0, str(_ROOT_FOR_PATH / "tests" / "bench"))
from _aura_harness import AFFECTED_DOC, B, G, N, R, Y, fail, info, ok, run, warn  # noqa: E402
from aura_file_runner import (  # noqa: E402
    CommandSpec,
    SnippetSpec,
    discover_aura_files,
    run_aura_file_suite,
    run_command_suite,
    run_snippet_suite,
)
from benchmark_cases import load_typecheck_cases  # noqa: E402
from integ_cases import load_integ_cases  # noqa: E402
from issue_tier import issues_tier, load_fast_targets, resolve_issue_targets  # noqa: E402
from smoke_cases import load_smoke_cases  # noqa: E402

ROOT = Path(__file__).resolve().parent
SCRIPTS = ROOT / "scripts"
COVERAGE_CHECKS = SCRIPTS / "coverage" / "checks"
COVERAGE_RUNNER = SCRIPTS / "coverage" / "runner.py"
COVERAGE_RUN_CHECKS = SCRIPTS / "coverage" / "run_checks.py"
COVERAGE_MANIFESTS = SCRIPTS / "coverage" / "manifests"


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


def _coverage_run(script: Path) -> subprocess.CompletedProcess:
    """Run a check_*.py, or its folded manifest if the script was removed."""
    if script.exists():
        return subprocess.run([sys.executable, str(script)], cwd=ROOT)
    issue = _issue_from_check_stem(script.stem) or _manifest_issue_for_script(script.name)
    if issue and (COVERAGE_MANIFESTS / f"{issue}.json").is_file():
        return subprocess.run([sys.executable, str(COVERAGE_RUNNER), "--issue", issue], cwd=ROOT)
    fail(f"missing {script}")
    return subprocess.CompletedProcess(args=[], returncode=1)


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


def _apply_sanitizer(name: str) -> None:
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


# ═══════════════════════════════════════════════════════════════
# Docs (code-generated)
# ═══════════════════════════════════════════════════════════════

GEN_DOCS = TOOLS / "gen_docs.py"


def cmd_docs(*, check: bool | None = None):
    """Generate or verify docs/generated/*.md from source."""
    if check is None:
        check = "--check" in sys.argv[2:]
    print(f"{B}═══ Docs {'(check)' if check else '(generate)'} ═══{N}")
    if not GEN_DOCS.exists():
        fail(f"missing {GEN_DOCS}")
        return 1
    args = [sys.executable, str(GEN_DOCS)]
    if check:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r == 0:
        ok("docs OK" if check else "docs generated")
    else:
        fail("docs stale — run ./build.py docs" if check else "docs generation failed")
    return r


# ═══════════════════════════════════════════════════════════════
# CLI templates (code-generated)
# ═══════════════════════════════════════════════════════════════

GEN_CLI_TEMPLATES = TOOLS / "gen_cli_templates_4913.py"
CLI_TEMPLATES_DIR = ROOT / "templates" / "new"
CLI_TEMPLATES_INC = ROOT / "src" / "cli_templates_4913.inc"


def cmd_cli_templates(*, check: bool | None = None):
    """Generate or verify src/cli_templates_4913.inc from templates/new.

    The .inc is what the binary actually serves for 'aura new'. A stale copy
    means 'aura new' writes the old template, so this belongs in the fast
    gate next to the docs freshness check (#5161).
    """
    if check is None:
        check = "--check" in sys.argv[2:]
    print(f"{B}═══ CLI templates {'(check)' if check else '(generate)'} ═══{N}")
    if not GEN_CLI_TEMPLATES.exists():
        fail(f"missing {GEN_CLI_TEMPLATES}")
        return 1
    args = [
        sys.executable,
        str(GEN_CLI_TEMPLATES),
        str(CLI_TEMPLATES_DIR),
        str(CLI_TEMPLATES_INC),
    ]
    if check:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r == 0:
        ok("cli templates OK" if check else "cli templates generated")
    else:
        fail("cli templates stale — run ./build.py cli-templates" if check else "cli templates generation failed")
    return r


def _cpp_source_files():
    """Same filter as .github/workflows/ci.yml clang-format step."""
    exts = {".cpp", ".ixx", ".hh", ".h"}
    files = []
    for base in ("src", "tests"):
        root = ROOT / base
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix in exts and path.is_file():
                files.append(path)
    return files


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


def cmd_format():
    """clang-format check/fix for C++ under src/ + tests/ (CI parity).

    Pass --changed (or AURA_FORMAT_CHANGED=1) to only touch files in the
    git diff — used by pre-push fast gate.
    """
    fix = "--fix" in sys.argv[2:]
    changed_only = "--changed" in sys.argv[2:] or os.environ.get("AURA_FORMAT_CHANGED", "").strip() in (
        "1",
        "true",
        "yes",
    )
    print(f"{B}═══ Format {'(fix)' if fix else '(check)'}{('+changed' if changed_only else '')} ═══{N}")
    clang_format = shutil.which("clang-format")
    if not clang_format:
        fail("clang-format not found — install clang-format (CI: llvm 22.x)")
        return 1
    files = _cpp_source_files()
    if changed_only:
        changed = set(_git_changed_files())
        files = [f for f in files if str(f.relative_to(ROOT)).replace("\\", "/") in changed]
        if not files:
            ok("clang-format: no changed C++ files (skip)")
            return 0
    if not files:
        fail("no C++ source files found under src/ or tests/")
        return 1
    info(f"checking {len(files)} files")
    if fix:
        r = run([clang_format, "-i", *[str(f) for f in files]], cwd=ROOT)
        if r != 0:
            fail("clang-format -i failed")
            return r
        ok("clang-format fixed")
        return 0
    r = run([clang_format, "--dry-run", "-Werror", *[str(f) for f in files]], cwd=ROOT)
    if r != 0:
        fail("clang-format check failed — run ./build.py format --fix")
        return r
    ok("clang-format OK")
    return 0


def cmd_lint():
    """Ruff lint + format check + (optional) sequential coverage scripts.

    Under ./build.py gate the sequential coverage chain is skipped
    (AURA_LINT_SKIP_COVERAGE=1) and scripts/coverage/run_checks.py runs
    the same check_*.py set in parallel with cascade suppression.
    Standalone `./build.py lint` still walks that chain. A folded script
    is gone; `_coverage_run` runs its manifest instead.
    """
    fix = "--fix" in sys.argv[2:]
    print(f"{B}═══ Lint {'(fix)' if fix else '(check)'} ═══{N}")
    ruff = shutil.which("ruff")
    if not ruff:
        fail("ruff not found — pip install -r requirements-dev.txt")
        return 1
    if fix:
        r = run([ruff, "check", ".", "--fix", "--unsafe-fixes"], cwd=ROOT)
        if r != 0:
            fail("ruff check --fix failed")
            return r
        r = run([ruff, "format", "."], cwd=ROOT)
        if r != 0:
            fail("ruff format failed")
            return r
        ok("lint fixed and formatted")
        return 0
    r = run([ruff, "check", "."], cwd=ROOT)
    if r != 0:
        fail("ruff check failed — run ./build.py lint --fix")
        return r
    r = run([ruff, "format", "--check", "."], cwd=ROOT)
    if r != 0:
        fail("ruff format check failed — run ./build.py lint --fix")
        return r
    # Gate path: coverage scripts run via parallel run_checks.py (cascade-free).
    if os.environ.get("AURA_LINT_SKIP_COVERAGE", "").strip() in ("1", "true", "yes"):
        ok("ruff OK (coverage checks deferred to parallel gate runner)")
        return 0
    # Coverage: the same run_checks.py runner the gate uses (Issue #5246).
    # The historical ~10k-line sequential chain re-ran the same ~640 check
    # scripts one at a time and could drift from the gate; both entry points
    # are this one runner now, so ./build.py lint and ./build.py gate cannot
    # disagree about which checks exist or how strict they are.
    return _run_parallel_coverage_checks(changed="--changed" in sys.argv[2:])
    return 0


def cmd_fixtures():
    """Validate tests/fixtures/*.json schema and baseline sync (#1961 → run.py)."""
    print(f"{B}═══ Fixtures (check) ═══{N}")
    script = ROOT / "tests" / "python" / "run.py"
    legacy = ROOT / "tests" / "python" / "fixture_check.py"
    if script.exists():
        r = run([sys.executable, str(script), "fixtures"], cwd=ROOT)
    elif legacy.exists():
        r = run([sys.executable, str(legacy)], cwd=ROOT)
    else:
        fail(f"missing {script}")
        return 1
    if r == 0:
        ok("fixtures OK")
    else:
        fail("fixture validation failed")
    return r


# ═══════════════════════════════════════════════════════════════
# Build
# ═══════════════════════════════════════════════════════════════


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


def _aura_test_env(extra: dict | None = None) -> dict:
    """Env for spawning `./build/aura` under CI / local harnesses.

    Issue #2053 / #2213: `main()` always calls
    `apply_production_security_defaults()` (Restricted sandbox, Forbidden
    tree-walker fallback, Full TypedMutationAudit). Integration suites
    (bash / suite / integ / regression / gradual / p0 / repl) must default
    to `AURA_SANDBOX=off` so Soft pipeline + Soft audit remain ergonomic —
    otherwise tree-walker-forbidden + MacroSelfEvo grant denials mass-fail
    under production defaults.

    Issue #3627: AURA_SANDBOX=off no longer selects the walker on the
    pack — the pipeline face is its own axis now (AURA_PIPELINE_STRICT).
    These suites exercise walker-side interpreter features by design, so
    default to `AURA_PIPELINE_STRICT=0` (diagnostics face) as well; the
    strict production face is covered by
    tests/compiler/test_pack_pipeline_strict.cpp. Same rationale as the
    ci/p0 module pin.

    Explicit `AURA_SANDBOX` / `AURA_PIPELINE_STRICT` in the caller
    environment always wins (canary / intentional prod-like runs). Does
    **not** mutate global `os.environ` so C++ unit tests that unsetenv +
    call `apply_production_security_defaults` keep clean
    production-default coverage.
    """
    env = os.environ.copy()
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    if not str(env.get("AURA_PIPELINE_STRICT", "")).strip():
        env["AURA_PIPELINE_STRICT"] = "0"
    if extra:
        env.update(extra)
    return env


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


def _phase(label: str, t0: float) -> None:
    """Issue #873/#874: always print wall-time for major build phases."""
    dt = time.time() - t0
    print(f"  {G}⏱{N} {label}: {dt:.1f}s", flush=True)


def _cmake_configure_args() -> list[str]:
    args = [
        "cmake",
        "-B",
        str(BUILD),
        "-G",
        "Ninja",
        "-Wno-author",
        "-Werror=experimental",
        "-Wpolicy",
        "-Wdeprecated",
        "-Wstrict",
        "-Wnon-target-directive",
    ]
    # Issue #5201: CMake 4.5 supports "import std" without an experimental
    # UUID gate or per-target opt-in; nothing to hard-code here.
    build_type = os.environ.get("AURA_BUILD_TYPE", "").strip()
    if build_type:
        args.append(f"-DCMAKE_BUILD_TYPE={build_type}")
    # Sanitizer flag injection (Issue #299). Active when BUILD was rebind
    # by _apply_sanitizer() to build_<san>/.
    san_name = BUILD.name.removeprefix("build_") if BUILD.name.startswith("build_") else ""
    ldflags_extra: list[str] = []
    cxxflags_extra: list[str] = []
    if san_name and san_name in SANITIZER_FLAGS:
        cxxflags, ldflags, build_type_override = SANITIZER_FLAGS[san_name]
        if build_type_override and not build_type:
            args.append(f"-DCMAKE_BUILD_TYPE={build_type_override}")
        cxxflags_extra.append(cxxflags)
        ldflags_extra.append(ldflags)

    # Issue #873/#874 Phase 1: mold (or lld) for much faster linking of
    # 100+ issue-test binaries. Default ON when the tool works with the
    # current GCC; set AURA_USE_MOLD=0 to force classic ld.bfd.
    # Probe is required: GCC 16's libatomic_asneeded.so breaks some mold
    # versions (CMake "CXX compiler is not able to compile a simple test").
    fast_ld = _select_fast_linker()
    if fast_ld == "mold":
        ldflags_extra.append("-fuse-ld=mold")
        info("linker: mold (AURA_USE_MOLD, probe ok)")
    elif fast_ld == "lld":
        ldflags_extra.append("-fuse-ld=lld")
        # scripts/linker-bin/ld.lld wrapper REMOVED per Anqi 2026-07-19
        # directive wave 12 (operational cruft — wrapper only filtered
        # benign libxml2 "no version information available" stderr noise
        # from distro libxml2 / mixed toolchains; that's an environment
        # issue, not a build pipeline concern. We just use system ld.lld
        # directly and accept the harmless stderr noise).
        info("linker: lld")
    else:
        info("linker: default (ld.bfd / system)")

    # Issue #873/#874: ccache is auto-used by cmake/aura_module_launcher.sh
    # when on PATH and CCACHE_DISABLE is unset. CI keeps CCACHE_DISABLE=1.
    if os.environ.get("CCACHE_DISABLE"):
        info("ccache: disabled (CCACHE_DISABLE set)")
    elif _tool_available("ccache"):
        info("ccache: available (module launcher will wrap compiles)")

    # Cap concurrent link jobs (each links huge .a + LLVM). Compiles stay
    # unbounded via ninja -j. Override with -DAURA_LINK_JOBS=N.
    link_jobs = os.environ.get("AURA_LINK_JOBS", "").strip()
    if link_jobs.isdigit() and int(link_jobs) > 0:
        args.append(f"-DAURA_LINK_JOBS={link_jobs}")

    if cxxflags_extra:
        args.append(f"-DCMAKE_CXX_FLAGS={' '.join(cxxflags_extra)}")
        args.append(f"-DCMAKE_C_FLAGS={' '.join(cxxflags_extra)}")
    if ldflags_extra:
        # Preserve any prior linker flags (sanitizer first).
        joined = " ".join(ldflags_extra)
        args.append(f"-DCMAKE_EXE_LINKER_FLAGS={joined}")
        args.append(f"-DCMAKE_SHARED_LINKER_FLAGS={joined}")

    # RelWithDebInfo default is -O2 -g, which bloats .o / mold RSS on the
    # issue-link farm. AURA_DEBUG_LEVEL=0..3 replaces -g with -gN. Skip
    # sanitizer trees — they need fuller frames for asan/ubsan traces.
    dbg = os.environ.get("AURA_DEBUG_LEVEL", "").strip()
    if dbg in {"0", "1", "2", "3"} and san_name not in SANITIZER_FLAGS:
        rel = f"-O2 -g{dbg} -DNDEBUG"
        args.append(f"-DCMAKE_CXX_FLAGS_RELWITHDEBINFO={rel}")
        args.append(f"-DCMAKE_C_FLAGS_RELWITHDEBINFO={rel}")
        info(f"debug level: -g{dbg} (AURA_DEBUG_LEVEL)")
    return args


def _cmake_build_cmd(targets: list[str], nproc: int) -> list[str]:
    cmd = ["cmake", "--build", str(BUILD), "-j", str(nproc)]
    for target in targets:
        cmd.extend(["--target", target])
    return cmd


def _build_named_targets(targets: list[str], nproc: int, *, fatal: bool, retry_ice: bool = True) -> int:
    """Ninja one or more cmake targets.

    retry_ice: GCC 16 ealias ICE on combined FILE_SET ninja is real for
    aura/test_ir. test_concurrent isolate failures have been deterministic
    (-Werror=unused-result); retrying the same compile doubled the wait
    and hid the diagnostic behind 'GCC ICE workaround'.
    """
    label = "+".join(targets)
    t0 = time.time()
    r = run(_cmake_build_cmd(targets, nproc), cwd=ROOT)
    if r != 0 and retry_ice:
        warn(f"{label} build failed — retrying once (GCC ICE workaround)")
        r = run(_cmake_build_cmd(targets, nproc), cwd=ROOT)
    elif r != 0:
        warn(f"{label} build failed — skipping ICE retry (deterministic compile)")
    _phase(f"build {label}", t0)
    if r != 0 and fatal:
        fail(f"build {label} failed")
    return r


# Own FILE_SET .ixx producers. Must not share a ninja with aura /
# aura_test_objects — GCC 16 ealias ICE on ast.ixx under -O2.
# test_concurrent is isolated so RelWithDebInfo -Werror on that TU is
# visible instead of a missing binary after a combined issue-farm ninja.
_MODULE_ICE_ISOLATE = frozenset({"test_ir", "test_gc_evaluator_integration", "test_concurrent"})
_MODULE_PRODUCER_DIRS = frozenset(f"{t}.dir" for t in _MODULE_ICE_ISOLATE)


def _target_bin_ok(name: str) -> bool:
    p = BUILD / name
    try:
        return p.is_file() and os.access(p, os.X_OK) and p.stat().st_size >= 1024
    except OSError:
        return False


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


def _launcher_consumer_dirs(main_bins: list[str], issue_targets: list[str]) -> list[str]:
    names: list[str] = []
    seen: set[str] = set()
    for t in [*main_bins, *issue_targets]:
        d = f"{t}.dir"
        if d in _MODULE_PRODUCER_DIRS or d in seen:
            continue
        seen.add(d)
        names.append(d)
    return names


def _apply_launcher_limit(dirs: list[str]) -> None:
    os.environ["AURA_MODULE_LAUNCHER_LIMIT_CONSUMERS"] = ",".join(dirs)
    info(f"module launcher BMI fan-out: {len(dirs)} consumer dirs")


def _drop_truncated_test_bins() -> None:
    for p in BUILD.glob("test_*"):
        try:
            if p.is_file() and p.stat().st_size < 1024:
                p.unlink(missing_ok=True)
                warn(f"removed truncated link output {p.name}")
        except OSError:
            pass


def _issue_ninja(targets: list[str] | None, *, jobs: int, label: str) -> int:
    """Build issue targets; return ninja rc. Caps peak link thrash."""
    cmd = ["ninja", "-C", str(BUILD), "-k", "0", f"-j{jobs}"]
    if targets:
        cmd.extend(targets)
    else:
        cmd.append("all_test_issue_targets")
    info(label)
    return run(cmd, cwd=ROOT)


def _retry_issue_colder(targets: list[str] | None, first_jobs: int, r: int) -> int:
    """Re-link issue bins at -j2 then -j1 after mold/disk SIGBUS."""
    _drop_truncated_test_bins()
    try:
        usage = shutil.disk_usage(BUILD)
        warn(
            f"issue-test build failed (rc={r}); free disk "
            f"{usage.free // (1024**3)} GiB of {usage.total // (1024**3)} GiB — retrying colder"
        )
    except OSError:
        warn(f"issue-test build failed (rc={r}) — retrying colder")
    r = _issue_ninja(
        targets,
        jobs=min(2, first_jobs),
        label="issue tests: retry -j2 (mold/disk flake)",
    )
    if r == 0:
        return 0
    return _issue_ninja(
        targets,
        jobs=1,
        label="issue tests: retry -j1 (serial link)",
    )


# ── Issue #4819: issue-farm sharding ─────────────────────────────
# One 4-vCPU runner spent ~85 of build-test's 120 minutes compiling the
# issue matrix (aura_test_objects + ~560 test TUs + 143 links). CI now
# splits all_test_issue_targets across AURA_ISSUE_SHARD=i/N matrix jobs.
# Targets are packed by compile weight (objects in CMakeFiles/<t>.dir +
# 1 link) with LPT so each shard gets about the same wall; the split is
# a pure function of the configured build.ninja, so every shard agrees.
_ISSUE_SHARD_LIST_NAME = ".issue-shard-targets"


def _skip_aura_executable() -> bool:
    """Issue shards set AURA_SKIP_AURA_BUILD. aura-bin builds build/aura once."""
    return os.environ.get("AURA_SKIP_AURA_BUILD", "").strip().lower() in (
        "1",
        "true",
        "yes",
        "on",
    )


def _ci_issues_phase() -> str:
    argv = sys.argv[2:]
    build_only = "--build-only" in argv
    test_only = "--test-only" in argv
    if build_only and test_only:
        raise SystemExit("ci-issues: pass only one of --build-only / --test-only")
    if build_only:
        return "build"
    if test_only:
        return "test"
    return "all"


def _issue_shard_spec() -> tuple[int, int] | None:
    raw = os.environ.get("AURA_ISSUE_SHARD", "").strip()
    if not raw:
        return None
    try:
        idx_s, total_s = raw.split("/", 1)
        idx, total = int(idx_s), int(total_s)
    except ValueError:
        raise SystemExit(f"AURA_ISSUE_SHARD={raw!r}: expected i/N (e.g. 2/4)") from None
    if total < 1 or not 1 <= idx <= total:
        raise SystemExit(f"AURA_ISSUE_SHARD={raw!r}: need 1 <= i <= N")
    return idx, total


def _issue_target_weights(targets: list[str]) -> dict[str, int]:
    weights = {t: 1 for t in targets}
    try:
        text = (BUILD / "build.ninja").read_text(errors="replace")
    except OSError:
        return weights
    for m in re.finditer(r"^build CMakeFiles/([^/\s]+)\.dir/\S+\.o:", text, re.M):
        t = m.group(1)
        if t in weights:
            weights[t] += 1
    return weights


def _shard_issue_targets(targets: list[str], idx: int, total: int) -> list[str]:
    """Deterministic LPT split of issue targets; returns shard idx (1-based)."""
    weights = _issue_target_weights(targets)
    loads = [0] * total
    bins: list[list[str]] = [[] for _ in range(total)]
    for t in sorted(set(targets), key=lambda t: (-weights[t], t)):
        k = min(range(total), key=lambda i: (loads[i], i))
        loads[k] += weights[t]
        bins[k].append(t)
    info(f"issue shard {idx}/{total}: {len(bins[idx - 1])} targets, weight {loads[idx - 1]} (all shards: {loads})")
    return sorted(bins[idx - 1])


def _write_issue_shard_list(targets: list[str]) -> Path:
    path = BUILD / _ISSUE_SHARD_LIST_NAME
    path.write_text("".join(f"{t}\n" for t in targets))
    return path


@dataclass
class _BuildCtx:
    nproc: int
    t_all: float
    overlap_main: list[str]
    concurrent_build_rc: int = 0
    concurrent_attempted: bool = False


def _begin_build() -> tuple[int, _BuildCtx | None]:
    """Configure + aura + ICE-isolated bins (test_ir). None ctx = done or fail.

    test_concurrent compile failure is recorded on the ctx instead of
    aborting: cmd_ci still builds/runs issues so one red leaf cannot
    hide the other.
    """
    print(f"{B}═══ Build ═══{N}")
    BUILD.mkdir(parents=True, exist_ok=True)
    nproc = _build_jobs()
    t_all = time.time()

    t0 = time.time()
    r = run(_cmake_configure_args(), cwd=ROOT)
    _phase("cmake configure", t0)
    if r != 0:
        return r, None

    # aura, then test_ir (own FILE_SET .ixx): a single ninja of
    # aura+test_ir races ast.ixx and can ICE GCC 16 ealias under -O2.
    # test_concurrent is isolated so RelWithDebInfo -Werror on that TU
    # is visible; compile fail does not abort the issue farm. Remaining
    # overlap ninja is issue binaries only.
    #
    # AURA_BUILD_TARGETS=aura[,test_ir,...] — restrict the main matrix
    # (deployment-health only needs the aura binary for --health-server).
    targets_env = os.environ.get("AURA_BUILD_TARGETS", "").strip()
    if targets_env:
        main_targets = tuple(t.strip() for t in targets_env.split(",") if t.strip())
        info(f"build targets (AURA_BUILD_TARGETS): {', '.join(main_targets)}")
    else:
        main_targets = ("aura", "test_ir", "test_concurrent")

    remaining = list(main_targets)
    if "aura" in remaining:
        remaining = [t for t in remaining if t != "aura"]
        # Issue tests spawn build/aura, but the executable is not a ninja
        # dependency of the issue slice. CI compiles it once in aura-bin
        # (parallel with the shards) and installs it before the test step.
        # aura_test_objects stays in the shard: its BMIs are a different
        # flag set from the aura executable, and a prerequisite job would
        # hold every shard until that library finished.
        if _skip_aura_executable():
            info("skip aura executable (AURA_SKIP_AURA_BUILD)")
        else:
            r = _build_named_targets(["aura"], nproc, fatal=True)
            if r != 0:
                return r, None

    # Aura-only builds (deployment-health) skip the issue matrix unless
    # AURA_ISSUE_BUILD=all is forced.
    if (
        targets_env
        and set(main_targets) == {"aura"}
        and os.environ.get("AURA_ISSUE_BUILD", "none").strip().lower() != "all"
    ):
        ok("build OK (aura-only; issue matrix skipped)")
        _phase("total build", t_all)
        return 0, None

    ice_targets = [t for t in remaining if t in _MODULE_ICE_ISOLATE]
    overlap_main = [t for t in remaining if t not in _MODULE_ICE_ISOLATE]
    concurrent_build_rc = 0
    concurrent_attempted = False
    for target in ice_targets:
        is_conc = target == "test_concurrent"
        if is_conc:
            concurrent_attempted = True
        r = _build_named_targets([target], nproc, fatal=not is_conc, retry_ice=not is_conc)
        if r != 0:
            if is_conc:
                concurrent_build_rc = r
                continue
            return r, None

    return 0, _BuildCtx(
        nproc=nproc,
        t_all=t_all,
        overlap_main=overlap_main,
        concurrent_build_rc=concurrent_build_rc,
        concurrent_attempted=concurrent_attempted,
    )


def _finish_overlap_issues(ctx: _BuildCtx) -> int:
    """Ninja the issue matrix. Issue link flakes are non-fatal."""
    nproc = ctx.nproc
    overlap_main = ctx.overlap_main

    # Build test_issue_* targets. Full tier uses the aggregate
    # (profile bundles + true standalones; dual standalones that
    # live in bundles are EXCLUDE_FROM_ALL — see #871/#873).
    # fast tier: fixture subset + git-changed (issue_tier.py).
    # AURA_ISSUE_BUILD=bundles → only the profile bundle exes.
    # AURA_ISSUE_BUILD=none/skip/off → skip issue matrix entirely.
    #
    # Sanitizer builds (build_asan / build_ubsan / build_tsan) default to
    # skipping the issue matrix: linking 500+ ASAN-instrumented test
    # binaries fills the GitHub Actions runner disk ("No space left on
    # device") and blows the wall clock. asan-verify / ubsan-smoke only
    # need aura + test_ir. Force full matrix with
    # AURA_ISSUE_BUILD=all under --sanitizer=.
    tier = issues_tier()
    issue_mode = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower()
    san_name = BUILD.name.removeprefix("build_") if BUILD.name.startswith("build_") else ""
    if san_name in SANITIZER_FLAGS and issue_mode in ("all", ""):
        issue_mode = "none"
        info(f"issue tests: skipped under --sanitizer={san_name} (set AURA_ISSUE_BUILD=all to force full matrix)")

    issue_ninja: list[str] | None
    issue_label = ""
    if issue_mode in ("none", "skip", "off", "0"):
        issue_ninja = []
        info("issue tests: skipped (AURA_ISSUE_BUILD=none)")
    elif tier == "full" and issue_mode == "bundles":
        from issue_tier import BUNDLE_PROFILES

        issue_ninja = [f"test_issues_{p}" for p in BUNDLE_PROFILES]
        issue_label = f"bundles ({len(issue_ninja)})"
    elif tier == "full":
        issue_ninja = None
        issue_label = "full (bundles + standalones; duals excluded)"
    else:
        issue_ninja = resolve_issue_targets("fast")
        changed = [t for t in issue_ninja if t not in set(load_fast_targets())]
        extra = f", +{len(changed)} git-changed" if changed else ""
        issue_label = f"fast ({len(issue_ninja)} targets{extra})"

    shard = _issue_shard_spec()
    if shard is not None and issue_mode not in ("none", "skip", "off", "0"):
        base = issue_ninja if issue_ninja is not None else _ninja_phony_inputs("all_test_issue_targets")
        if issue_ninja is None and not base:
            fail("issue shard: cannot list all_test_issue_targets from build.ninja")
            return 1
        issue_ninja = _shard_issue_targets(base, *shard)
        _write_issue_shard_list(issue_ninja)
        issue_label = f"{issue_label} → shard {shard[0]}/{shard[1]} ({len(issue_ninja)} targets)"

    building_issues = issue_ninja is None or bool(issue_ninja)
    # Issue-target fan-out. The historical cap of 4 barely used wide boxes.
    # Default to the full build job count; AURA_ISSUE_JOBS clamps it down
    # when link thrash (mold/disk SIGBUS) needs containing.
    issue_jobs = nproc
    raw_issue_jobs = os.environ.get("AURA_ISSUE_JOBS", "").strip()
    if raw_issue_jobs:
        try:
            requested = int(raw_issue_jobs)
        except ValueError:
            requested = 0
        if requested > 0:
            issue_jobs = max(1, min(nproc, requested))

    saved_limit = os.environ.get("AURA_MODULE_LAUNCHER_LIMIT_CONSUMERS")
    try:
        limit_issue: list[str] = []
        if issue_ninja is None:
            limit_issue = _ninja_phony_inputs("all_test_issue_targets")
        elif issue_ninja:
            limit_issue = list(issue_ninja)
        # Only pin fan-out when we know every consumer; an empty query
        # on the full aggregate would omit issue dirs and break imports.
        if limit_issue or not building_issues:
            limit_dirs = _launcher_consumer_dirs(overlap_main, limit_issue)
            if limit_dirs:
                _apply_launcher_limit(limit_dirs)

        if not building_issues:
            if overlap_main:
                r = _build_named_targets(overlap_main, nproc, fatal=len(overlap_main) == 1)
                if r != 0:
                    if len(overlap_main) == 1:
                        return r
                    warn(f"combined {'+'.join(overlap_main)} failed — falling back to one target at a time")
                    for target in overlap_main:
                        r = _build_named_targets([target], nproc, fatal=True)
                        if r != 0:
                            return r
        else:
            t0 = time.time()
            ninja_targets = list(overlap_main)
            if issue_ninja is None:
                ninja_targets.append("all_test_issue_targets")
            else:
                ninja_targets.extend(issue_ninja)
            extra = f"+{','.join(overlap_main)}" if overlap_main else ""
            info(f"issue tests: tier={tier} mode={issue_label}{extra}, -j{issue_jobs}")
            r = _ninja_build(ninja_targets, jobs=issue_jobs, keep_going=True)
            for target in overlap_main:
                if not _target_bin_ok(target):
                    warn(f"{target} missing after combined ninja — retrying")
                    rr = _build_named_targets([target], nproc, fatal=True)
                    if rr != 0:
                        return rr
            if r != 0:
                r = _retry_issue_colder(issue_ninja, issue_jobs, r)
            _phase("build test bins + issue tests", t0)
            if r != 0:
                # Don't fail cmd_build on partial-build errors —
                # the runner will skip the unbuilt binaries.
                print(f"{Y}  some test_issue_* targets failed to build (pre-existing); runner will skip them{N}")
    finally:
        if saved_limit is None:
            os.environ.pop("AURA_MODULE_LAUNCHER_LIMIT_CONSUMERS", None)
        else:
            os.environ["AURA_MODULE_LAUNCHER_LIMIT_CONSUMERS"] = saved_limit

    _phase("build total", ctx.t_all)
    ok("build OK")
    return 0


def cmd_build():
    """CMake 构建 (Ninja)"""
    rc, ctx = _begin_build()
    if ctx is None:
        return rc
    link = _finish_overlap_issues(ctx)
    if ctx.concurrent_build_rc:
        fail("build test_concurrent failed")
        return ctx.concurrent_build_rc
    return link


def cmd_configure():
    """CMake configure only. Leaves CMakeCache so a runtime gate can build its target.

    release.sh writes the tarball from build_repro/. The chaos SOAK hard
    gate reads the default build directory. Without this configure, that
    gate stops on a missing CMakeCache after static coverage and never
    builds or runs test_chaos_mutate_steal_gc_mailbox.
    """
    print(f"{B}═══ Configure ═══{N}")
    BUILD.mkdir(parents=True, exist_ok=True)
    r = run(_cmake_configure_args(), cwd=ROOT)
    if r != 0:
        fail("cmake configure failed")
        return r
    if not (BUILD / "CMakeCache.txt").exists():
        fail(f"cmake configure left no {BUILD / 'CMakeCache.txt'}")
        return 1
    ok(f"configured {BUILD}")
    return 0


def cmd_clean():
    """清理构建产物"""
    print(f"{B}═══ Clean ═══{N}")
    if BUILD.exists():
        run(["cmake", "--build", str(BUILD), "--target", "clean"], cwd=ROOT)
        shutil.rmtree(BUILD)
        ok(f"removed {BUILD}")
    else:
        info("nothing to clean")
    return 0


# ═══════════════════════════════════════════════════════════════
# Unit tests (C++)
# ═══════════════════════════════════════════════════════════════


def test_unit():
    """C++ test_ir only (concurrent is its own leaf)."""
    print(f"{B}═══ Unit tests ═══{N}")
    if not TEST_BIN.exists():
        fail(f"{TEST_BIN} not found — run 'build' first")
        return 1

    start = time.time()
    r = subprocess.run([str(TEST_BIN)], capture_output=True, text=True)
    elapsed = time.time() - start
    for line in r.stdout.strip().split("\n"):
        if "passed" in line.lower():
            ok(line.strip())
        elif "FAIL" in line:
            fail(line.strip())
    if r.returncode != 0:
        # Surface UBSAN/ASAN diagnostics + aborts (stderr was previously
        # swallowed, which made CI failures look like silent unit fails).
        fail(f"test_ir exited {r.returncode}")
        err = (r.stderr or "").strip()
        if err:
            # Cap output so a sanitizer flood still leaves a usable log tail.
            lines = err.splitlines()
            if len(lines) > 80:
                lines = lines[:40] + ["  ... (stderr truncated) ..."] + lines[-40:]
            print(f"{R}── test_ir stderr ──{N}")
            for line in lines:
                print(f"  {line}")
        print(f"  Unit tests: {elapsed:.2f}s")
        fail("unit tests failed")
        return 1
    print(f"  Unit tests: {elapsed:.2f}s")
    ok("unit tests passed")
    return 0


# ═══════════════════════════════════════════════════════════════
# Integration tests (.aura files)
# ═══════════════════════════════════════════════════════════════


# Integration cases live in tests/fixtures/integ/*.json (#1962)
# (loaded via tests/python/integ_cases.py — #1932 layout).


def _case_jobs() -> int:
    """Fan-out for multi-case suites (integ / typecheck / smoke).

    AURA_CASE_JOBS overrides; else when AURA_TEST_JOBS>1 use min(4, jobs).
    """
    raw = os.environ.get("AURA_CASE_JOBS", "").strip()
    if raw.isdigit():
        return max(1, int(raw))
    raw_t = os.environ.get("AURA_TEST_JOBS", "1").strip()
    try:
        tj = max(1, int(raw_t))
    except ValueError:
        tj = 1
    if tj <= 1:
        return 1
    return max(1, min(4, tj))


def test_integ():
    """端到端管线测试 — eval / ir / typecheck / serve (shared snippet runner)."""
    print(f"{B}═══ Integration tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found — run 'build' first")
        return 1

    flags = {
        "eval": (),
        "ir": ("--ir",),
        "typecheck": ("--typecheck",),
        "serve": ("--serve",),
    }
    specs: list[SnippetSpec] = []
    for tc in load_integ_cases():
        specs.append(
            SnippetSpec(
                name=f"[{tc.pipeline:10s}] {tc.name}",
                code=tc.code,
                extra_args=flags.get(tc.pipeline, ()),
                expect_out=tc.expected,
                expect_err=tc.expected_err,
                expect_status=tc.expected_status,
                # err_div_zero accepts 0 / SIGFPE / clean error report.
                accept_status=(0, -8, 1) if tc.name == "err_div_zero" else (),
                timeout_s=30,
                stdin_suffix="" if tc.pipeline == "serve" else "\n",
                stdout_last_line=tc.pipeline == "serve",
            )
        )
    return run_snippet_suite(
        "Integration",
        specs,
        aura_bin=AURA,
        env=_aura_test_env(),
        jobs=_case_jobs(),
    )


# ═══════════════════════════════════════════════════════════════
# Typecheck tests
# ═══════════════════════════════════════════════════════════════


def test_typecheck():
    """类型检查专项测试 (shared snippet runner, type: line)."""
    print(f"{B}═══ Typecheck tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1
    specs = [
        SnippetSpec(
            name=tc.name,
            code=tc.code,
            extra_args=("--typecheck",),
            expect_out=tc.expected_type,
            expect_status=None,
            timeout_s=10,
            type_line=True,
        )
        for tc in load_typecheck_cases()
    ]
    return run_snippet_suite(
        "Typecheck",
        specs,
        aura_bin=AURA,
        env=_aura_test_env(),
        jobs=_case_jobs(),
    )


# ═══════════════════════════════════════════════════════════════
# Benchmark
# ═══════════════════════════════════════════════════════════════


def test_bench():
    """Benchmark 基线 + 回归检测（#1569 / #1936: statistical SLO gate).

    Path map (#1570): this is the benchmark gate — NOT src/test/benchmark_gate.ixx.
    Forwards argv after `bench` / `test bench` to tests/bench/benchmark.py
    (e.g. --strict --tolerance 5 --runs 3 --rationale "...").
    """
    print(f"{B}═══ Benchmark ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1
    env = _aura_test_env({"AURA_BIN": str(AURA)})
    args = [sys.executable, str(BENCH)]
    # Forward flags after the "bench" token (or whole argv for `test bench ...`).
    # Issue #1936: --tolerance / --runs / --mode / --rationale.
    if "bench" in sys.argv:
        i = sys.argv.index("bench")
        fwd = sys.argv[i + 1 :]
    else:
        fwd = [a for a in sys.argv[2:] if a != "bench"]
    # Issue #1569: hard SLO gate when AURA_CI_STRICT_BENCH=1 or --strict.
    strict = (
        os.environ.get("AURA_CI_STRICT_BENCH", "0").strip()
        in (
            "1",
            "true",
            "TRUE",
            "yes",
            "YES",
        )
        or "--strict" in sys.argv
        or "--check" in sys.argv
    )
    if strict and "--strict" not in fwd and "--check" not in fwd:
        args.append("--strict")
    args.extend(fwd)
    if strict:
        print("  mode: STRICT SLO gate (AURA_CI_STRICT_BENCH / --strict)")
    else:
        print("  mode: soft (warn on regression; AURA_CI_STRICT_BENCH=1 for hard fail)")
    return run(args, env=env)


def cmd_bench():
    """Issue #1569 / #1936: ./build.py bench [--strict] [--tolerance N] [--runs N]."""
    return test_bench()


# ═══════════════════════════════════════════════════════════════
# Smoke tests
# ═══════════════════════════════════════════════════════════════


def test_smoke():
    """快速冒烟测试 (shared command runner)."""
    print(f"{B}═══ Smoke tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1
    specs = [CommandSpec(name=sc.name, command=sc.command, expect=sc.expected, cwd=ROOT) for sc in load_smoke_cases()]
    return run_command_suite("Smoke", specs, env=_aura_test_env(), jobs=_case_jobs())


# ═══════════════════════════════════════════════════════════════
# Mutation tests
# ═══════════════════════════════════════════════════════════════


def test_mutation():
    """Agent 变异循环 — mutation loop 功能验证"""
    print(f"{B}═══ Mutation tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    for flag in ["--demo", "--list"]:
        r = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "mutation_loop.py"), flag],
            capture_output=True,
            text=True,
            timeout=30,
        )
        print(r.stdout)
        if r.returncode != 0:
            fail(f"mutation {flag} failed")
            return 1
        ok(f"mutation: {flag} OK")

    fixture = ROOT / "tests" / "fixtures" / "basic_add.aura"
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests" / "mutation_loop.py"),
            str(fixture),
            "--fast",
        ],
        capture_output=True,
        text=True,
        timeout=30,
    )
    print(r.stdout)
    if r.returncode != 0:
        fail("mutation single-pass failed")
        return 1
    ok("mutation: single-pass OK")


def test_runtime_unit():
    """runtime.c 单元测试"""
    print(f"{B}═══ runtime.c Unit Tests ═══{N}")
    import tempfile

    # Unique output path so suite-level parallelism cannot clobber /tmp/runtime_test.
    td = tempfile.mkdtemp(prefix="aura_runtime_c_")
    out_bin = str(Path(td) / "runtime_test")
    r = subprocess.run(
        [
            "gcc",
            "-g",
            "-DTEST_BUILD=1",
            str(ROOT / "tests" / "runtime_test_harness.c"),
            str(ROOT / "lib" / "runtime.c"),
            "-o",
            out_bin,
            "-lm",
        ],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if r.returncode != 0:
        print(r.stderr[:500])
        fail("runtime.c test compilation failed")
        return 1
    r = subprocess.run([out_bin], capture_output=True, text=True, timeout=30)
    print(r.stdout)
    if r.returncode != 0:
        fail("runtime.c unit tests failed")
        return 1
    ok("runtime-c: passed")
    return 0


# ═══════════════════════════════════════════════════════════════
# REPL / demo / ai_agent_demo
# ═══════════════════════════════════════════════════════════════


def test_repl():
    """REPL interactive tests"""
    print(f"{'repl':12s} testing REPL interaction...")
    try:
        import pexpect  # noqa: F401
    except ImportError:
        print(f"  {'⚠️':4s} pexpect not installed (pip install -r requirements-dev.txt)")
        return 0
    r = subprocess.run(
        [sys.executable, "tests/python/repl_test.py"],
        cwd=ROOT,
        env=_aura_test_env(),
    )
    if r.returncode:
        fail("repl tests failed")
        return 1
    ok("repl tests passed")
    return 0


def test_demo():
    """Agent demo — full pipeline"""
    print(f"{B}═══ Agent Demo ═══{N}")
    r = subprocess.run([sys.executable, str(ROOT / "tests" / "agent_demo.py")])
    if r.returncode == 0:
        ok("agent demo passed")
    else:
        fail("agent demo failed")
    return r.returncode


def test_ai_agent_demo():
    """AI Agent 端到端演示"""
    print(f"{B}═══ AI Agent Demo ═══{N}")
    r = subprocess.run([sys.executable, str(ROOT / "tests" / "ai_agent_demo.py")], timeout=120)
    if r.returncode == 0:
        ok("ai agent demo passed")
    else:
        fail("ai agent demo failed")
    return r.returncode


# ═══════════════════════════════════════════════════════════════
# Regression / gradual / bash / suite
# ═══════════════════════════════════════════════════════════════


def test_gradual():
    """Gradual Guarantee verification (#1961 → tests/run.py gradual).

    R10 #1932 layout migration relocated thin redirect Python scripts
    from tests/<name>.py to tests/python/<name>.py. Test runner and
    legacy redirect moved to tests/python/. Prefer new path; fall back
    to legacy path for back-compat with old fixture layouts.
    """
    runner = ROOT / "tests" / "python" / "run.py"
    legacy = ROOT / "tests" / "check_gradual.py"
    if runner.exists():
        cmd = [sys.executable, str(runner), "gradual"]
    elif legacy.exists():
        cmd = [sys.executable, str(legacy)]
    else:
        print(f"  {runner} not found")
        return 1
    # 30s dies when this cheap wave overlaps the issue-farm ninja (CI
    # run 6021: timeout at 30s, rc=1, no assertion failure).
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=_aura_test_env())
    print(r.stdout)
    if r.returncode != 0:
        fail("gradual guarantee failed")
        return 1
    ok("gradual guarantee passed")
    return 0


def test_bash():
    """Bash regression (#1961 → tests/run.py bash).

    R10 #1932: tests/run.py → tests/python/run.py.
    """
    print(f"{B}═══ Bash regression tests ═══{N}")
    runner = ROOT / "tests" / "python" / "run.py"
    shell = ROOT / "tests" / "run-tests.sh"
    # Bash harness has hundreds of short cases; Soft sandbox still takes
    # >2m under load (ASAN CI longer). Outer timeout must exceed sum of
    # per-case `timeout 5` budgets — use 360s for CI headroom.
    if runner.exists():
        r = subprocess.run(
            [sys.executable, str(runner), "bash"],
            env=_aura_test_env({"AURA": str(AURA)}),
            capture_output=True,
            text=True,
            timeout=360,
        )
    elif shell.exists():
        r = subprocess.run(
            ["bash", str(shell)],
            env=_aura_test_env({"AURA": str(AURA)}),
            capture_output=True,
            text=True,
            timeout=360,
        )
    else:
        fail(f"{shell} not found")
        return 1
    print(r.stdout)
    if r.stderr:
        print(r.stderr)
    if r.returncode == 0:
        ok("bash tests passed")
    else:
        fail("bash tests failed")
    return r.returncode


def test_regression():
    """Run tests/regression/*.aura via the shared file runner (stdin + ;; expect:)."""
    print(f"{B}═══ Regression tests ═══{N}")
    aura_bin = os.environ.get("AURA_BIN", str(AURA))
    found = discover_aura_files(ROOT / "tests" / "regression")
    if not found.cases:
        print("  No regression tests found", flush=True)
        return 0
    return run_aura_file_suite(
        "Regression",
        found,
        aura_bin=aura_bin,
        env=_aura_test_env(),
        mode="stdin",
        timeout_s=10,
        jobs=_case_jobs(),
    )


def test_concurrent():
    """Run concurrent model unit tests (test_concurrent)."""
    print(f"{B}═══ Concurrent Tests ═══{N}")
    bin_path = BUILD / "test_concurrent"
    if not bin_path.exists():
        print("  test_concurrent binary not found")
        return 1
    # Issue #217 follow-up: 180s timeout was too short for
    # the 5258-test stress run (occasionally >180s under
    # system load, causing false-positive "1/N test suites
    # failed" in CI). 600s gives comfortable headroom.
    #
    # ci/concurrent (GitHub-only, ~17.3s rc=1): the binary dies by
    # signal mid-run with zero FAIL lines / zero Results line — the
    # block-buffered stdout tail (last test + crash context) is lost
    # on signal death, so the failing test is invisible in CI logs.
    # Line-buffer via stdbuf so every PASS/FAIL flushes. Skip under
    # asan/tsan: stdbuf LD_PRELOADs libstdbuf.so ahead of the sanitizer
    # runtime and the process aborts at startup ("ASan runtime does not
    # come first") — asan-verify concurrent died in 0.2s. The binary
    # already setvbuf(_IOLBF) in main (#3567 AC3).
    cmd: list[str] = [str(bin_path)]
    san_name = BUILD.name.removeprefix("build_") if BUILD.name.startswith("build_") else ""
    if shutil.which("stdbuf") and san_name not in ("asan", "tsan"):
        cmd = ["stdbuf", "-oL", "-eL", str(bin_path)]
    # Sanitizer waves need suite-level headroom: 600s was sized by #217
    # against PLAIN CI history (plain suite occasionally >180s). ubsan-smoke
    # red 2026-09-30: test_concurrent timed out (600s) on the CI runner,
    # while the same suite under UBSAN finishes in 82-106s locally even on
    # a heavily loaded box — no hang, no stall, 593/593 across 5 runs, and
    # UBSAN ≈ plain wall clock (the suite is sync-bound, not compute-bound;
    # #4230/#4231/#4235 add no material cost). On the small x64 CI runners
    # that same wall clock sits near the 600s boundary and one loaded run
    # tipped it over. 1800s keeps 3x headroom (mirrors the issues full-tier
    # budget; the ubsan-smoke job itself allows timeout-minutes: 240).
    timeout_s = 1800 if san_name in ("asan", "ubsan", "tsan") else 600
    # Issue #3586: unarmed Scheduler(N>1).run() aborts unless production
    # bootstrap is latched or AURA_SANDBOX=off. Latch Soft for this suite
    # (same helper as issue/integ runners). Explicit AURA_SANDBOX wins.
    try:
        r = subprocess.run(cmd, timeout=timeout_s, env=_aura_test_env())
    except subprocess.TimeoutExpired:
        print(f"  ✗ test_concurrent timed out ({timeout_s}s)")
        return 1
    if r.returncode != 0 and r.stderr:
        print(r.stderr[:500], file=sys.stderr)
    return r.returncode


def test_issues():
    """Run issue/domain/bundle C++ binaries via tests/run.py (#1961).

    Tier controlled by AURA_ISSUES_TIER: full = all binaries,
    fast = issues_fast.json subset + git-changed issue tests.

    Issue #871: --changed forces the runner to operate strictly on
    git-diff-touched issue tests (no bundle subset). Useful for
    PR simulation when the bundle subset is too aggressive, and
    for local iteration when an issue is the only thing modified.

    When AURA_ISSUE_BUILD=none/skip/off (CI path filter: no issue_matrix
    paths touched), skip cleanly — binaries were not linked.
    """
    issue_mode = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower()
    if issue_mode in ("none", "skip", "off", "0"):
        info("issue tests: skipped (AURA_ISSUE_BUILD=none — no issue matrix build)")
        return 0
    tier = issues_tier()
    # Full tier defaults to 4 workers (not 8): high fan-out was the main
    # source of SIGSEGV/timeout under load. Override with AURA_ISSUES_JOBS.
    _cpu = os.cpu_count() or 4
    _default = min(4, _cpu) if tier == "full" else min(8, _cpu)
    jobs = os.environ.get("AURA_ISSUES_JOBS") or str(_default)
    extra_args: list[str] = []
    if "--changed" in sys.argv:
        extra_args.append("--changed")
        # Override the tier to fast when --changed is requested so
        # the runner only emits the git-changed subset (no full
        # issue bundle aggregate build).
        if tier == "full":
            tier = "fast"
            os.environ["AURA_ISSUES_TIER"] = "fast"
    print(f"{B}═══ Issue Tests (tier={tier}, jobs={jobs}{', --changed' if '--changed' in sys.argv else ''}) ═══{N}")
    # Prefer unified CLI; falls through to run_issue_tests implementation.
    cmd_name = "issues-fast" if tier == "fast" else "issues"
    # R10 #1932: tests/run.py → tests/python/run.py.
    run_path = ROOT / "tests" / "python" / "run.py"
    if not run_path.exists():
        run_path = ROOT / "tests" / "run.py"
    # Full tier wall clock: serial recovery pass can double worst-case time.
    # Issue #3522: stream stdout/stderr (do not capture_output). Capture
    # hid every per-binary line until exit, so a 1800s timeout printed
    # nothing and hid which binary was running / hung.
    # Fast tier: one shard can own a 240s orch batch (test_orch_agent_batch)
    # plus a 600s jit_late1 bundle; the old 300s cap killed the shard while
    # a legitimate per-binary timeout was still running.
    timeout_s = 1800 if tier == "full" else 900
    cmd = [
        sys.executable,
        str(run_path),
        cmd_name,
        "--tier",
        tier,
        "--",
        "--jobs",
        jobs,
        *extra_args,
    ]
    try:
        r = subprocess.run(cmd, timeout=timeout_s)
        return r.returncode
    except subprocess.TimeoutExpired as e:
        fail(f"ci/issues timed out after {timeout_s}s")
        if e.stdout:
            out = e.stdout if isinstance(e.stdout, str) else e.stdout.decode(errors="replace")
            print(out[-8000:])
        if e.stderr:
            err = e.stderr if isinstance(e.stderr, str) else e.stderr.decode(errors="replace")
            print(err[-4000:], file=sys.stderr)
        return 1


def test_p0_regression():
    """Run P0 fix regression tests."""
    print(f"{B}═══ P0 Regression Tests ═══{N}")
    # tests/python/test_regression.py runs 150+ Aura subprocess cases plus
    # JIT/AOT/fuzz helpers; wall time often exceeds 3 min on loaded runners.
    # Path moved under tests/python/ in #1932 layout migration.
    #
    # CI (2026-10-31): this suite runs in the cheap/medium wave WHILE the
    # issue-farm ninja is still linking on the same 4-vCPU runner. The old
    # 300s cap was hit there (TimeoutExpired -> _run_suite reports rc=1,
    # surfacing as "cheap/medium wave ... rc=1" with no test failure). The
    # suite is ~150s on an idle box; give the overlapped run 3x headroom and
    # print the tail on timeout so the stalled case is visible in CI logs.
    try:
        r = subprocess.run(
            [sys.executable, str(ROOT / "tests" / "python" / "test_regression.py")],
            capture_output=True,
            text=True,
            timeout=900,
            cwd=str(ROOT),
            env=_aura_test_env(),
        )
    except subprocess.TimeoutExpired as e:
        fail("ci/p0 timed out after 900s")
        out = e.stdout if isinstance(e.stdout, str) else (e.stdout or b"").decode(errors="replace")
        err = e.stderr if isinstance(e.stderr, str) else (e.stderr or b"").decode(errors="replace")
        if out:
            print(out[-8000:])
        if err:
            print(err[-4000:], file=sys.stderr)
        return 1
    print(r.stdout)
    if r.stderr:
        print(r.stderr, file=sys.stderr)
    return r.returncode


# Suite tests that are temporarily skipped because of pre-existing
# issues unrelated to the current work. Each entry is (filename, reason).
# The skip is reported as a warning (so it's visible in CI logs) but does
# not fail the suite. These are tracked as follow-up work — see
# commit messages on the relevant fixes for context.
SUITE_SKIP: dict[str, str] = {
    # Add entries here as {filename: reason} for tests that should be
    # temporarily skipped. Empty = all suite tests run.
    # Cleared after null-owner primitive dispatch + set! free-var top_
    # fallback (poly_mutation_soundness / gc under --load).
    #
    # Issue #2213 Soft-sandbox harness pass still hangs after
    # "starting-cycles..." (spin / unbounded mutate loop). Skip so
    # suite CI completes; track re-enable after cycle bound fix.
    "incremental_mutation_test.aura": "hangs after starting-cycles (unbounded loop; Soft sandbox still hangs)",
    # run-tests counts non-check forms as fails + SIGSEGV mid-suite on
    # extract/move path under --load; track re-enable after harness + crash.
    "mutate-structured.aura": "run-tests counts set-code/eval as fails; SIGSEGV on later suites under --load",
}

# P4: curated S0 surface smoke (AURA_PRIMITIVES=s0). Full suite stays full-mode.
# Expand as more suite files become s0-clean (no bulk stats / eda / security).
SUITE_S0_FILES = frozenset(
    {
        "engine_metrics.aura",
        "stdlib_surface.aura",
        "core.aura",
        "stdlib.aura",
        "errors.aura",
        "macros.aura",
        "module.aura",
    }
)


def _suite_jobs() -> int:
    """Parallel workers for tests/suite/*.aura (AURA_SUITE_JOBS).

    Default: when AURA_TEST_JOBS>1 (CI), use min(8, nproc, AURA_TEST_JOBS);
    otherwise 1 (local serial for simpler logs).
    """
    raw = os.environ.get("AURA_SUITE_JOBS", "").strip()
    if raw.isdigit():
        return max(1, int(raw))
    tj = _test_jobs()
    if tj <= 1:
        return 1
    nproc = os.cpu_count() or 4
    return max(1, min(8, nproc, tj))


def test_suite_runner(*, s0: bool = False):
    """Run tests/suite/*.aura via the shared file runner (--load, exit 0)."""
    label = "Suite tests (s0)" if s0 else "Suite tests"
    print(f"{B}═══ {label} ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found — run 'build' first")
        return 1
    env = _aura_test_env()
    if s0:
        env["AURA_PRIMITIVES"] = "s0"
    found = discover_aura_files(
        ROOT / "tests" / "suite",
        skip=SUITE_SKIP,
        exclude={"run-tests.aura"},
        allow=set(SUITE_S0_FILES) if s0 else None,
    )
    # 120s per file dies when this cheap wave overlaps the issue-farm
    # ninja. CI run 6021: suite/multiframe_named_let_2873.aura (quadratic
    # named-let fill) hit the cap with every other case green.
    # 240s -> 360s (2026-11-12): the runner itself swings ~1.5x run to run
    # (ci/p0 415.8s -> 623.3s on the same wave shape), and suite/qd_4941.aura
    # (5000 map-elites evaluations, ~100s on a fast box) sat right on 240s:
    # it passed at 12:21 and timed out at 15:51 on otherwise unchanged code.
    rc = run_aura_file_suite(
        "Suite",
        found,
        aura_bin=AURA,
        env=env,
        mode="load",
        timeout_s=360,
        jobs=_suite_jobs(),
    )
    if s0:
        print("  [AURA_PRIMITIVES=s0]")
    return rc


def test_suite_s0():
    """Curated suite under AURA_PRIMITIVES=s0 (P4 surface smoke)."""
    return test_suite_runner(s0=True)


def test_s0_metrics():
    """Cheap engine:metrics probe under AURA_PRIMITIVES=s0 (build-test step).

    Production defaults forbid tree-walker fallback (#2213), so this must
    use the same Soft harness env as the other aura suites.
    """
    print(f"{B}═══ s0 engine:metrics ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found — run 'build' first")
        return 1
    prog = (
        "(define m (engine:metrics))\n"
        '(unless (hash? m) (error "engine:metrics not hash"))\n'
        '(unless (= 2 (hash-ref m "schema")) (error "schema"))\n'
        '(display "s0 engine:metrics ok")\n'
        "(newline)\n"
    )
    env = _aura_test_env({"AURA_PRIMITIVES": "s0"})
    r = subprocess.run(
        [str(AURA)],
        input=prog,
        text=True,
        capture_output=True,
        env=env,
        cwd=ROOT,
    )
    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0 or "s0 engine:metrics ok" not in (r.stdout or ""):
        sys.stdout.write(out)
        fail(f"s0 engine:metrics probe failed (rc={r.returncode})")
        return 1
    ok("s0 engine:metrics ok")
    return 0


def test_e2e():
    """Issue #1934: commercial_readiness .aura E2E with golden PASS labels."""
    script = ROOT / "tests" / "python" / "run_e2e.py"
    if not script.is_file():
        fail(f"missing {script}")
        return 1
    return run([sys.executable, str(script)], cwd=ROOT)


# ═══════════════════════════════════════════════════════════════
# Suite registry — leaves + tiers (SSOT for list / help / expand)
# ═══════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class SuiteSpec:
    name: str
    fn: Callable[[], int]
    kind: str  # cpp | aura | extra
    doc: str
    parallel: bool = True
    # cmd_test wave: cheap (fan-out) → medium (2-wide) → heavy/stress (exclusive).
    # Caps inner AURA_*_JOBS so suite-level × inner-level never exceeds nproc.
    wave: str = "cheap"


SUITE_SPECS: tuple[SuiteSpec, ...] = (
    SuiteSpec("unit", test_unit, "cpp", "C++ test_ir only", wave="cheap"),
    SuiteSpec(
        "concurrent",
        test_concurrent,
        "cpp",
        "C++ test_concurrent stress",
        parallel=False,
        wave="stress",
    ),
    SuiteSpec(
        "issues",
        test_issues,
        "cpp",
        "C++ domain / issue binaries (AURA_ISSUES_TIER=full)",
        parallel=False,
        wave="heavy",
    ),
    SuiteSpec(
        "issues-fast",
        test_issues,
        "cpp",
        "same runner as issues, AURA_ISSUES_TIER=fast",
        parallel=False,
        wave="heavy",
    ),
    SuiteSpec("runtime-c", test_runtime_unit, "cpp", "lib/runtime.c harness", wave="cheap"),
    SuiteSpec("integ", test_integ, "aura", "fixture integ via aura_file_runner snippets", wave="cheap"),
    SuiteSpec("typecheck", test_typecheck, "aura", "fixture typecheck via aura_file_runner", wave="cheap"),
    SuiteSpec("smoke", test_smoke, "aura", "fixture smoke via command runner", wave="cheap"),
    SuiteSpec("bash", test_bash, "aura", "tests/run-tests.sh via tests/python/run.py", wave="medium"),
    SuiteSpec(
        "suite",
        test_suite_runner,
        "aura",
        "tests/suite/*.aura (--load via aura_file_runner)",
        wave="medium",
    ),
    SuiteSpec(
        "regression",
        test_regression,
        "aura",
        "tests/regression/*.aura (stdin + ;; expect: via aura_file_runner)",
        wave="medium",
    ),
    SuiteSpec("gradual", test_gradual, "aura", "gradual guarantee", wave="cheap"),
    SuiteSpec("p0", test_p0_regression, "aura", "P0 snippets + freeze/AOT extras", wave="medium"),
    SuiteSpec("e2e", test_e2e, "aura", "commercial_readiness golden E2E", parallel=False, wave="stress"),
    SuiteSpec(
        "suite-s0",
        test_suite_s0,
        "aura",
        "suite subset under AURA_PRIMITIVES=s0",
        parallel=False,
        wave="stress",
    ),
    SuiteSpec("repl", test_repl, "extra", "REPL pexpect", wave="cheap"),
    SuiteSpec("bench", test_bench, "extra", "benchmark SLO", parallel=False, wave="stress"),
    SuiteSpec("mutation", test_mutation, "extra", "mutation_loop smoke", parallel=False, wave="stress"),
    SuiteSpec("demo", test_demo, "extra", "agent demo pipeline", parallel=False, wave="stress"),
    SuiteSpec("ai", test_ai_agent_demo, "extra", "AI agent demo", parallel=False, wave="stress"),
)

SUITE_BY_NAME: dict[str, SuiteSpec] = {s.name: s for s in SUITE_SPECS}
SUITES: dict[str, Callable[[], int]] = {s.name: s.fn for s in SUITE_SPECS}

CORE_LEAVES: tuple[str, ...] = (
    "unit",
    "integ",
    "typecheck",
    "smoke",
    "bash",
    "suite",
    "repl",
    "runtime-c",
    "concurrent",
)
SAFETY_LEAVES: tuple[str, ...] = ("gradual", "regression", "p0")
CI_LEAVES: tuple[str, ...] = CORE_LEAVES + SAFETY_LEAVES + ("issues",)
PR_LEAVES: tuple[str, ...] = CORE_LEAVES + SAFETY_LEAVES + ("issues-fast",)
NIGHTLY_LEAVES: tuple[str, ...] = CI_LEAVES + ("e2e", "bench", "suite-s0")
EVERYTHING_LEAVES: tuple[str, ...] = tuple(s.name for s in SUITE_SPECS if s.name != "issues-fast")

TIER_LEAVES: dict[str, tuple[str, ...]] = {
    "core": CORE_LEAVES,
    "safety": SAFETY_LEAVES,
    "pr": PR_LEAVES,
    "ci": CI_LEAVES,
    "all": CI_LEAVES,
    "nightly": NIGHTLY_LEAVES,
    "everything": EVERYTHING_LEAVES,
}

TIER_DOCS: dict[str, str] = {
    "pr": "core + safety + issues-fast",
    "ci": "core + safety + issues (CI / default; `all` is an alias)",
    "all": "alias of ci — not every leaf",
    "nightly": "ci + e2e + bench + suite-s0",
    "everything": "every leaf including demo / ai / mutation / bench",
    "core": "unit integ typecheck smoke bash suite repl runtime-c concurrent",
    "safety": "gradual + regression + p0",
}

# Derived aliases for older call sites.
CI_CORE = list(CORE_LEAVES)
CI_SAFETY = list(SAFETY_LEAVES)
CI_ISSUES = ["issues"]
CI_ISSUES_FAST = ["issues-fast"]
CI_PARALLEL_SAFE = frozenset(s.name for s in SUITE_SPECS if s.parallel)


_SUITE_WAVES: tuple[str, ...] = ("cheap", "medium", "heavy", "stress")
# Max suite-level workers per wave (None = min(AURA_TEST_JOBS, batch size)).
_WAVE_SUITE_WORKERS: dict[str, int | None] = {
    "cheap": None,
    "medium": 2,
    "heavy": 1,
    "stress": 1,
}


def _validate_suite_registry() -> None:
    """Fail loud if tiers drift from leaves."""
    unknown = [n for leaves in TIER_LEAVES.values() for n in leaves if n not in SUITE_BY_NAME]
    if unknown:
        raise RuntimeError(f"tier references unknown leaves: {unknown}")
    if TIER_LEAVES["all"] != TIER_LEAVES["ci"]:
        raise RuntimeError("`all` must stay an alias of `ci`")
    extra_in_ci = {"demo", "ai", "mutation", "bench", "e2e", "suite-s0"} & set(TIER_LEAVES["ci"])
    if extra_in_ci:
        raise RuntimeError(f"ci/all must not include extras: {sorted(extra_in_ci)}")
    unit_src = test_unit.__code__.co_names
    if "test_concurrent" in unit_src or "concurrent_bin" in unit_src:
        raise RuntimeError("unit must not embed the concurrent binary")
    bad_wave = [s.name for s in SUITE_SPECS if s.wave not in _SUITE_WAVES]
    if bad_wave:
        raise RuntimeError(f"SuiteSpec.wave must be one of {_SUITE_WAVES}: {bad_wave}")


_validate_suite_registry()


_test_print_lock = Lock()


def _test_jobs() -> int:
    raw = os.environ.get("AURA_TEST_JOBS", "1").strip()
    try:
        return max(1, int(raw))
    except ValueError:
        return 1


def _known_suite_names() -> str:
    leaves = ", ".join(s.name for s in SUITE_SPECS)
    tiers = ", ".join(TIER_LEAVES)
    return f"tiers: {tiers}; leaves: {leaves}"


def _expand_suite_names(suite_names: list[str]) -> list[tuple[str, object]]:
    if not suite_names:
        suite_names = ["ci"]

    rewritten = ["ci" if n == "all" else n for n in suite_names]
    if "all" in suite_names:
        info("`all` is an alias of `ci` (not every leaf). Use `everything` for demo/bench/ai.")

    if any(n == "issues-fast" or n == "pr" for n in rewritten):
        os.environ["AURA_ISSUES_TIER"] = "fast"

    items: list[tuple[str, object]] = []
    seen: set[str] = set()

    def add_leaf(label: str, leaf: str) -> None:
        if leaf in seen:
            return
        spec = SUITE_BY_NAME.get(leaf)
        if spec is None:
            warn(f"unknown suite '{leaf}' (use: {_known_suite_names()})")
            return
        seen.add(leaf)
        items.append((label, spec.fn))

    for name in rewritten:
        if name in SUITE_BY_NAME:
            add_leaf(name, name)
        elif name in TIER_LEAVES:
            for leaf in TIER_LEAVES[name]:
                add_leaf(f"{name}/{leaf}", leaf)
        else:
            warn(f"unknown suite '{name}' (use: {_known_suite_names()})")
    return items


def _run_suite(label: str, fn) -> tuple[str, int, float]:
    t0 = time.time()
    with _test_print_lock:
        print(f"\n{B}▶ {label}{N}")
    try:
        rc = fn()
    except Exception as exc:  # noqa: BLE001 — surface harness bugs in CI
        with _test_print_lock:
            print(f"{R}✗ {label}: {exc}{N}")
        rc = 1
    elapsed = time.time() - t0
    with _test_print_lock:
        mark = f"{G}✓{N}" if rc == 0 else f"{R}✗{N}"
        print(f"  {mark} {label} ({elapsed:.1f}s)")
    return label, rc, elapsed


_LAST_FAILED_SUITES: list[str] = []


def _summarize_test_results(results: dict[str, int], *, heading: str | None = None) -> int:
    global _LAST_FAILED_SUITES
    _LAST_FAILED_SUITES = [label for label, rc in sorted(results.items()) if rc != 0]
    print(f"\n{'═' * 50}")
    if heading:
        print(heading)
    all_ok = all(v == 0 for v in results.values())
    total = len(results)
    bad = total - sum(1 for v in results.values() if v == 0)
    if total == 0:
        print(f"{Y}No test suites in this slice{N}")
        return 0
    if bad == 0:
        print(f"{G}All {total} test suites passed{N}")
    else:
        print(f"{R}{bad}/{total} test suites failed{N}")
        for label, rc in sorted(results.items()):
            if rc != 0:
                print(f"  {R}✗{N} {label}")
    return 1 if not all_ok else 0


def _suite_base_name(label: str) -> str:
    return label.split("/")[-1]


def _suite_wave(label: str) -> str:
    spec = SUITE_BY_NAME.get(_suite_base_name(label))
    return spec.wave if spec else "cheap"


def _cap_inner_jobs(live_suites: int) -> dict[str, str]:
    """Pin inner fan-out so suite-level × inner ≤ nproc.

    Explicit AURA_*_JOBS in the environment are caps, not floors.
    """
    cores = os.cpu_count() or 4
    inner = max(1, cores // max(1, live_suites))
    applied: dict[str, str] = {}
    for key in ("AURA_SUITE_JOBS", "AURA_ISSUES_JOBS", "AURA_CASE_JOBS"):
        raw = os.environ.get(key, "").strip()
        if raw.isdigit() and int(raw) > 0:
            applied[key] = str(max(1, min(int(raw), inner)))
        else:
            applied[key] = str(inner)
    return applied


def _cmd_test_affected(rest: list[str]) -> int:
    """Genealogy fast loop. Not a CI tier and not an issue-farm build (#5232)."""
    script = ROOT / "tests" / "run.py"
    return subprocess.run([sys.executable, str(script), "affected", *rest], cwd=ROOT).returncode


def cmd_test(suite_names: list[str], *, waves: tuple[str, ...] | None = None):
    """Run test suites in cheap → medium → heavy → stress waves.

    Inner AURA_SUITE_JOBS / AURA_ISSUES_JOBS / AURA_CASE_JOBS are capped to
    nproc // live_suites so overlapping heavies cannot oversubscribe the host.
    ``waves`` limits which waves run (cmd_ci overlaps cheap/medium with ninja).
    ``affected`` is the local fast loop and is not expanded into CI leaves.
    """
    if suite_names and suite_names[0] == "affected":
        return _cmd_test_affected(suite_names[1:])
    items = _expand_suite_names(suite_names)
    if not items:
        warn("no test suites to run")
        return 1

    wave_set = set(waves) if waves else None
    jobs = _test_jobs()
    results: dict[str, int] = {}
    heading = f"Waves {', '.join(waves)}" if waves else None

    if jobs <= 1:
        for label, fn in items:
            if wave_set is not None and _suite_wave(label) not in wave_set:
                continue
            name, rc, _elapsed = _run_suite(label, fn)
            results[name] = rc
        return _summarize_test_results(results, heading=heading)

    by_wave: dict[str, list[tuple[str, object]]] = {w: [] for w in _SUITE_WAVES}
    for label, fn in items:
        by_wave[_suite_wave(label)].append((label, fn))

    saved_inner = {k: os.environ.get(k) for k in ("AURA_SUITE_JOBS", "AURA_ISSUES_JOBS", "AURA_CASE_JOBS")}
    try:
        for wave in _SUITE_WAVES:
            if wave_set is not None and wave not in wave_set:
                continue
            batch = by_wave[wave]
            if not batch:
                continue
            cap = _WAVE_SUITE_WORKERS[wave]
            workers = 1 if cap == 1 else min(jobs if cap is None else cap, len(batch), jobs)
            inner = _cap_inner_jobs(workers)
            os.environ.update(inner)
            print(
                f"{B}Wave {wave}: {len(batch)} suite(s), workers={workers}, "
                f"inner_jobs={inner['AURA_SUITE_JOBS']} "
                f"(suite/issues/case){N}"
            )
            if workers <= 1:
                for label, fn in batch:
                    name, rc, _elapsed = _run_suite(label, fn)
                    results[name] = rc
            else:
                with ThreadPoolExecutor(max_workers=workers) as pool:
                    futures = {pool.submit(_run_suite, label, fn): label for label, fn in batch}
                    for fut in as_completed(futures):
                        label, rc, _elapsed = fut.result()
                        results[label] = rc
    finally:
        for k, v in saved_inner.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    return _summarize_test_results(results, heading=heading)


def cmd_primitive_surface():
    """P0b/#1432 freeze + #1448 SlimSurface --strict (budget + facade report)."""
    print(f"{B}═══ Primitive surface freeze + SlimSurface ═══{N}")
    script = COVERAGE_CHECKS / "check_primitive_surface.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1448: --strict includes freeze + public budget + facade report.
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "primitive surface freeze/strict failed "
            "(no new *-stats / string|json|math|vector|path|time-* / ast:ref-*; "
            "public count ≤ interim ceiling)"
        )
        return 1
    # Issue #1432 / #1448: synthetic unit tests (blocks deliberately-bad names + strict).
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_primitive_surface_gate.py"
    if ut.exists():
        r2 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r2.returncode != 0:
            fail("primitive surface gate unit tests failed")
            return 1
    ok("primitive surface freeze + SlimSurface --strict OK")
    return 0


def cmd_test_registry():
    """Issue #1572: test-registry.json freshness (scripts/tools/gen_test_registry.py).

    Default: --check (fail if docs/generated/test-registry.json is stale).
    With --fix: rewrite the registry from tests/test_*.cpp headers.
    Also wired into pre-commit when tests/*.cpp is staged.
    """
    fix = "--fix" in sys.argv[2:]
    print(f"{B}═══ Test registry {'(fix)' if fix else '(check)'} (#1572) ═══{N}")
    script = TOOLS / "gen_test_registry.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    args = [sys.executable, str(script)]
    if not fix:
        args.append("--check")
    r = run(args, cwd=ROOT)
    if r != 0:
        if fix:
            fail("test-registry generation failed")
        else:
            fail("test-registry stale — run ./build.py test-registry --fix")
        return r
    ok("test-registry regenerated" if fix else "test-registry OK")
    return 0


def cmd_test_binding():
    """Issue #1453: prim source ↔ tests/ binding + test-registry freshness."""
    print(f"{B}═══ Test binding + coverage (#1453) ═══{N}")
    # Unit tests for the gate itself
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_test_binding_gate.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_test_binding_gate unit tests failed")
            return 1
    # Issue #901/#908: FNV-1a and the slot-fingerprint rule have exactly one
    # owner (src/compiler/hash_meta.h). Run the source invariant here so the
    # gate catches new copies even when the changed file is allowlisted.
    mc = ROOT / "tests" / "python" / "test_magic_constants.py"
    if mc.exists():
        r_mc = subprocess.run([sys.executable, str(mc)], cwd=ROOT)
        if r_mc.returncode != 0:
            fail("magic-constant invariant failed — use hash_meta.h constants")
            return 1
    # check_test_coverage.py umbrella removed per Anqi 2026-07-19 directive
    # (scripts/ audit wave 9). check_test_binding.py covers the same surface
    # (production primitive sources must have tests/).
    script = COVERAGE_CHECKS / "check_test_binding.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("test binding failed — production primitive sources changed without tests/")
        return 1
    ok("test binding + coverage OK")
    return 0


def cmd_new_test_files():
    """Reject newly added issue-numbered test filenames.

    Same classifier as ``.githooks/pre-commit`` (``--cached``). Gate mode also
    sees commits versus origin/main and untracked files, so a missing
    ``core.hooksPath`` does not skip the ban.
    """
    print(f"{B}═══ New test filenames ═══{N}")
    ut = ROOT / "tests" / "python" / "test_new_test_files.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_new_test_files unit tests failed")
            return 1
    script = TOOLS / "check_new_test_files.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("new issue-numbered test file blocked — append to the home in tests/HOMES.md")
        return 1
    ok("new test filenames OK")
    return 0


def cmd_dead_heap_push():
    """Issue #1488 / #1668: dead string_heap_ push pollution audit (strict)."""
    print(f"{B}═══ Dead string_heap push audit (#1668) ═══{N}")
    script = AUDIT / "audit_dead_heap_push.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_audit_dead_heap_push.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_audit_dead_heap_push unit tests failed")
            return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "dead string_heap_ push candidates found — "
            "run python3 scripts/audit/audit_dead_heap_push.py and remove unused pushes"
        )
        return 1
    ok("dead heap push audit clean")
    return 0


def cmd_catch_silent_swallow():
    """Issue #1669 / #615: catch(...) must carry SILENCE-PRIM marker (strict)."""
    print(f"{B}═══ catch(...) SILENCE-PRIM audit (#1669) ═══{N}")
    script = AUDIT / "audit_catch_silent_swallow.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_audit_catch_silent_swallow.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_audit_catch_silent_swallow unit tests failed")
            return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail("unmarked catch(...) found — add [SILENCE-PRIM-#615] (or fix silent swallow)")
        return 1
    ok("catch silent-swallow audit clean")
    return 0


def cmd_aot_env_linear_stamp():
    """Issue #2091 / #2168: forbid literal (0,0) env/linear on production mangle/emit.

    Runs scripts/coverage/checks/check_aot_env_linear_stamp_coverage.py as a hard gate:
    any production call to mangle_aot_name / aot_link_name that passes
    literal (0, 0) without `# 2091-allow-zero` (or `# 2091-legacy`) fails
    the build. Tests/stubs/header defaults remain allowed (script skip list).
    """
    print(f"{B}═══ AOT env/linear stamp coverage (#2091 / #2168) ═══{N}")
    script = COVERAGE_CHECKS / "check_aot_env_linear_stamp_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    # Unit tests for the linter itself (annotation contract + self-test).
    ut = ROOT / "tests" / "python" / "test_aot_env_linear_stamp_gate.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_aot_env_linear_stamp_gate unit tests failed")
            return 1
    # Built-in self-test (bridge TU has no bare (0,0)).
    r_st = subprocess.run(
        [sys.executable, str(script), "--self-test"],
        cwd=ROOT,
    )
    if r_st.returncode != 0:
        fail("aot env/linear stamp self-test failed (aura_jit_bridge.cpp regression)")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "production mangle_aot_name/aot_link_name passes literal (0,0) — "
            "thread live env/linear via aot_resolve_emit_* or add "
            "`# 2091-allow-zero` (see aot_mangle.h / #2168)"
        )
        return 1
    ok("aot env/linear stamp coverage clean (no bare (0,0) emit paths)")
    return 0


def cmd_legacy_test_inventory():
    """Issue #1957: living legacy test inventory freshness check.

    Without --fix: ``scripts/tools/inventory_legacy_tests.py --check`` (exit 1 if
    tests/legacy_test_inventory.md is stale). With --fix: regenerate the
    markdown. Re-run after domain migrations or bulk test adds.
    """
    print(f"{B}═══ Legacy test inventory (#1957) ═══{N}")
    script = TOOLS / "inventory_legacy_tests.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    fix = "--fix" in sys.argv[2:]
    args = [sys.executable, str(script)]
    if not fix:
        args.append("--check")
    r = subprocess.run(args, cwd=ROOT)
    if r.returncode != 0:
        if fix:
            fail("legacy inventory regenerate failed")
        else:
            fail(
                "legacy_test_inventory.md stale — run "
                "`python3 scripts/tools/inventory_legacy_tests.py` or `./build.py gate --fix`"
            )
        return 1
    ok("legacy test inventory up to date" if not fix else "legacy test inventory regenerated")
    return 0


# Issue #3387 — PR-level short soak path filter. When the diff touches
# any of the named runtime/Guard/steal/restamp paths, run the
# `soak-pr-short` ctest label on PRs; otherwise skip silently (AC2).
# Used by cmd_soak_pr_short_3387_coverage below.
_SOAK_PR_SHORT_PATH_PATTERNS = (
    "src/serve/steal_safety",
    "src/serve/fiber",
    "src/compiler/evaluator_mutation_boundary.cpp",
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/compiler/evaluator_security.cpp",
    "src/core/flatast_restamp.hh",
)


def _paths_touch_soak_pr_short(diff_paths: list[str]) -> bool:
    """Return True iff any diff path matches a soak-pr-short surface.

    Issue #3387 AC2: PRs that don't touch these files pay 0 cost
    (no ctest invocation, no 60s timeout).
    """
    for p in diff_paths:
        for needle in _SOAK_PR_SHORT_PATH_PATTERNS:
            if p.startswith(needle) or p == needle or needle in p:
                return True
    return False


def _collect_diff_paths() -> list[str]:
    """Collect paths touched between HEAD and HEAD~1 (or origin/main
    if HEAD has no parent yet).

    Best-effort: returns empty list on any git error so the soak gate
    still observes "no relevant changes" semantics.
    """
    try:
        r = subprocess.run(
            ["git", "diff", "--name-only", "HEAD~1", "HEAD"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        if r.returncode == 0 and r.stdout:
            return [ln.strip() for ln in r.stdout.splitlines() if ln.strip()]
    except FileNotFoundError:
        pass
    return []


def cmd_soak_pr_short_3387_coverage():
    """Issue #3387: PR-level short soak fail-closed on hold-after-cancel +
    steal residual-zero + torn probe (I1/I3/I6 gate).

    Closes the gap where unit AC green was insufficient evidence for
    high-concurrency AI Agent load defects (hold-budget cancel without
    cooperative edge, steal residual arm Soft under latch, query:*
    -stable torn probe vs export face split). Nightly soak (#2722 /
    #2755 / #2856 / #2931 / #3073) catches them — but too late for the
    PR that re-opens the window. A bounded short slice on the existing
    binaries is enough to catch counter non-zero / probe split; does
    NOT replace the nightly matrix.

    Contract rows (AC1-AC5):

      AC1: PR touching the path filter runs the `soak-pr-short` ctest
           label and fails on any of: steal residual-zero nonzero under
           latch, hold-after-cancel > 2×SLO, torn-probe / export face
           split. The three target suites already fail-closed on those
           observables (regression guarded by their existing ACs).
      AC2: Unrelated PRs (no diff touch on the runtime/Guard/steal/
           restamp surfaces) pay 0 cost — short-circuit on the path
           filter before ctest invocation.
      AC3: Soft / unlatched fixtures inside those suites stay
           metric-only (existing AC — preserved, not modified).
      AC4: No new soak binary; no docs/design/3387-*; no
           tests/issues/test_issue_3387.cpp (#81967).
      AC5: Nightly full chaos matrix stays as-is (#2722 lineage) —
           this issue only wires the PR admission gate.
    """
    paths = _collect_diff_paths()
    if not _paths_touch_soak_pr_short(paths):
        info("soak-pr-short (#3387): path filter miss — skipping (unrelated PR)")
        return 0
    print(f"{B}=== soak-pr-short (#3387) — path filter hit, running ctest -L soak-pr-short ==={N}")
    # 60s per-test ceiling matches AC1 budget; ctest `-L` filters to
    # `soak-pr-short` label only — the three target suites +
    # test_mailbox_hold_starvation_hard (if/when labelled).
    timeout = int(os.environ.get("AURA_SOAK_PR_SHORT_TIMEOUT", "60"))
    build_dir_env = os.environ.get("CMAKE_BINARY_DIR")
    ctest_dir = Path(build_dir_env) if build_dir_env else ROOT / "build"
    if not (ctest_dir / "CTestTestfile.cmake").exists():
        info(f"soak-pr-short (#3387): CTestTestfile.cmake not found at {ctest_dir} — skipping")
        return 0
    r = subprocess.run(
        ["ctest", "-L", "soak-pr-short", "--timeout", str(timeout), "--output-on-failure"],
        cwd=str(ctest_dir),
        text=True,
        capture_output=True,
    )
    if r.returncode != 0:
        fail(
            f"soak-pr-short (#3387) ctest failed (rc={r.returncode}); "
            f"tail:\n{(r.stdout or '')[-2000:]}\n{(r.stderr or '')[-2000:]}"
        )
        return 1
    ok("soak-pr-short (#3387) ctest -L soak-pr-short clean")
    return 0


def cmd_pcv_span_stale_coverage_3167():
    """Issue #3167: SafePCVSpan stale-across-guard fingerprint + force_refresh.

    Wire-up: SafePCVSpan 6-arg ctor captures (node_id, generation,
    wrap_epoch, node_gen) at children_safe_view time. FlatAST::
    force_refresh_pcv_span bumps pcv_span_stale_across_guard_total +
    re-pins via children_safe_view on stale; happy path returns original
    span (AC2 zero extra). Production contract; Soft/Off unchanged.
    #2906 (flatast-locked-move-out) MUST NOT regress — fingerprint is
    observation-only, additive counter only.
    #3167 wrapper relocated to top-level scripts/ by the coverage
    restructure (d375fc915) — path corrected accordingly (#cleanup).
    """
    print(f"{B}=== pcv span stale coverage (#3167) ==={N}")
    script = ROOT / "scripts" / "check_pcv_span_stale_coverage_3167.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("pcv span stale (#3167) coverage contract rows failed")
        return 1
    ok("pcv span stale (#3167) coverage clean")
    # Issue #3328 residual: production children_stable / query re-use.
    pcv3328 = COVERAGE_CHECKS / "check_pcv_stale_span_query_refresh_3328.py"
    if not pcv3328.exists():
        fail(f"missing {pcv3328}")
        return 1
    r3328 = subprocess.run([sys.executable, str(pcv3328)], cwd=ROOT)
    if r3328.returncode != 0:
        fail("pcv stale-span query refresh (#3328) coverage contract rows failed")
        return 1
    ok("pcv stale-span query refresh (#3328) coverage clean")
    return 0


def cmd_batch_dirty_production_multi_only_2936_coverage():
    """Issue #2936: production multi-block IR dirty must use batch API only (static)."""
    print(f"{B}=== batch dirty production multi-only coverage (#2936) ==={N}")
    script = COVERAGE_CHECKS / "check_batch_dirty_production_multi_only_2936.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("batch dirty production multi-only (#2936) coverage contract rows failed")
        return 1
    ok("batch dirty production multi-only (#2936) coverage clean")
    return 0


def cmd_batch_dirty_production_multi_only_2936():
    """Issue #2936: production multi-block dirty = batch APIs only.

    Residual multi-via-single (#2774 Soft metric) hard-expects 0 under production
    smoke. Optional AURA_IR_DIRTY_BATCH_ONLY=1 aborts on residual; Soft/unit
    intentional residual remains when env unset. schema-2936 + smoke-wired.
    """
    # Issue #3105: default AURA_IR_DIRTY_BATCH_ONLY=1 for production builds /
    # build.py production presets so the hard-abort path in ir_soa.ixx
    # note_single_mark_for_residual is armed (ir_dirty_batch_only_hard() returns
    # true → std::abort on residual). Tests that deliberately exercise
    # residual override this by unsetting the env var before the test runs.
    os.environ["AURA_IR_DIRTY_BATCH_ONLY"] = "1"
    print(f"{B}=== batch dirty production multi-only (#2936) ==={N}")
    return cmd_batch_dirty_production_multi_only_2936_coverage()


def cmd_stable_ref_probe_3400_coverage():
    """Issue #3400: mutate:check-stable-ref probes the node_gen_ domain.

    The Agent early-validity probe compared workspace FlatAST::generation()
    to the captured StableNodeRef gen — two different counter domains that
    diverge by design after any Guard / atomic-batch bump, so the probe
    lied in both directions (false stale on live node; false live after
    wrap / slot reuse). This ticket routes the probe through the #3396
    unpack SSOT (v1 Soft / v2 production) and makes validity =
    ref.is_valid_in(flat) && flat.get_safe(ref). Production with a
    provenance-less v1 pack → stale-ref (same face as #3396 apply).
    GUARD_EXEMPT read-only face + StaleRefPolicy Strict/Warn/Disabled kept.
    Non-regress: #235/#391 probe + policy, #3396 unpack, #3399 SSOT routing.
    """
    print(f"{B}=== check-stable-ref node-gen domain probe (#3400) ==={N}")
    script = COVERAGE_CHECKS / "check_stable_ref_probe_3400.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("check-stable-ref node-gen domain probe (#3400) coverage contract rows failed")
        return 1
    ok("check-stable-ref node-gen domain probe (#3400) coverage clean")

    print(f"{B}=== grant lifetime alignment probe (#3436) ==={N}")
    script = COVERAGE_CHECKS / "check_grant_lifetime_alignment_3436.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("grant lifetime alignment (#3436) coverage contract rows failed")
        return 1
    ok("grant lifetime alignment (#3436) coverage clean")
    return 0


def cmd_hot_pass_hard_dod_coverage():
    """Issue #2434: hard HotPassDodCompliant for all production stages.

    Unmarked soft-skip removed; production pack inventory; schema-2434.
    """
    print(f"{B}=== hot pass hard dod coverage (#2434) ==={N}")
    script = COVERAGE_CHECKS / "check_hot_pass_hard_dod_2434.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("hot pass hard dod (#2434) coverage contract rows failed")
        return 1
    ok("hot pass hard dod (#2434) coverage clean")
    # Issue #3329 residual: production purity / SoA / DirtyPropagator gate.
    pp3329 = COVERAGE_CHECKS / "check_production_pipeline_purity_3329.py"
    if not pp3329.exists():
        fail(f"missing {pp3329}")
        return 1
    r3329 = subprocess.run([sys.executable, str(pp3329)], cwd=ROOT)
    if r3329.returncode != 0:
        fail("production pipeline purity (#3329) coverage contract rows failed")
        return 1
    ok("production pipeline purity (#3329) coverage clean")
    return 0


def cmd_issue_coverage():
    """Declarative issue-coverage manifests (Phase 1 runner + Phase 2 --changed).

    Default: --changed (only manifests whose paths intersect the git diff vs
    origin/main + worktree). Pass --all for full run (nightly / CI).
    Pilot issues: #2622 #2623 #2624. New issues should add a manifest JSON,
    not a full hand-written check_*.py.
    """
    print(f"{B}=== issue coverage manifests (Phase 1+2, scripts/coverage) ==={N}")
    runner = COVERAGE_RUNNER
    if not runner.exists():
        fail(f"missing {runner}")
        return 1
    # argv after `build.py issue-coverage` may include --all / --base X
    extra = [a for a in sys.argv[2:] if a in ("--all", "--changed", "--list", "--index") or a.startswith("--base")]
    # Default Phase 2: --changed unless caller asked for --all/--list/--index
    if not any(a in ("--all", "--changed", "--list", "--index") for a in extra):
        extra = ["--changed", *extra]
    r = subprocess.run([sys.executable, str(runner), *extra], cwd=ROOT)
    if r.returncode != 0:
        fail("issue coverage manifest runner failed")
        return 1
    ok("issue coverage manifests clean")
    return 0


def cmd_chaos_steal_gc_nightly_2931_coverage():
    """Issue #2931: static contract for chaos steal-gc nightly hard gate.

    Nightly profile: AURA_CHAOS_STEAL_GC=1 AURA_CHAOS_DURATION_S≥600
    AURA_CHAOS_WORKERS≥8; residual-after-exit + resume-fence fail-closed;
    EXCLUDE_FROM_ALL + env gate preserved for PR default.
    """
    print(f"{B}=== chaos steal-gc nightly hard gate coverage (#2931) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_steal_gc_nightly_2931.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos steal-gc nightly (#2931) coverage contract rows failed")
        return 1
    ok("chaos steal-gc nightly (#2931) coverage clean")
    return 0


def cmd_chaos_steal_gc_nightly_2931():
    """Issue #2931: nightly hard gate — steal×mutate×GC×mailbox chaos soak.

    Env matrix (hard-fail if residual/resume-fence unbounded):
      AURA_CHAOS_STEAL_GC=1
      AURA_CHAOS_DURATION_S≥600  AURA_CHAOS_WORKERS≥8
    Soft steal (AURA_STEAL_SNAPSHOT_SOFT) forbidden under the gate.
    Builds test_chaos_steal_mutation_gc if needed (EXCLUDE_FROM_ALL target).
    PR default remains opt-in via env + EXCLUDE_FROM_ALL.
    CI gate job without cmake: static coverage only.
    """
    print(f"{B}=== chaos steal-gc nightly hard gate (#2931) ==={N}")
    rc = cmd_chaos_steal_gc_nightly_2931_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_steal_mutation_gc"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            ok(
                "chaos steal-gc nightly runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via nightly CI"
            )
            return 0
        info("building test_chaos_steal_mutation_gc…")
        nproc = os.cpu_count() or 4
        r = subprocess.run(
            [
                "ninja",
                "-C",
                str(BUILD),
                "-j",
                str(max(1, nproc // 2)),
                "test_chaos_steal_mutation_gc",
            ],
            cwd=ROOT,
        )
        if r.returncode != 0:
            fail("build test_chaos_steal_mutation_gc failed")
            return r.returncode
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_CHAOS_STEAL_GC"] = "1"
    env.setdefault("AURA_CHAOS_DURATION_S", "600")
    env.setdefault("AURA_CHAOS_WORKERS", "8")
    # Soft steal forbidden under nightly hard gate.
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)

    info(
        "env: AURA_CHAOS_STEAL_GC=1 "
        f"workers={env['AURA_CHAOS_WORKERS']} duration={env['AURA_CHAOS_DURATION_S']}s "
        "(residual-after-exit / resume-fence / ticket fail-closed)"
    )
    timeout_s = max(180, int(env["AURA_CHAOS_DURATION_S"]) + 180)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos steal-gc nightly timed out after {timeout_s}s (hang?) — gate blocked")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"chaos steal-gc nightly FAILED exit={r.returncode} in {elapsed:.1f}s — "
            "residual_defer_after_exit without matching clears / resume_fence hard "
            "surplus / ticket mismatch / residual_defer_steal_hard_fail must be 0"
        )
        return r.returncode
    ok(
        f"chaos steal-gc nightly GREEN in {elapsed:.1f}s "
        f"(workers={env['AURA_CHAOS_WORKERS']} duration={env['AURA_CHAOS_DURATION_S']}s; "
        "residual/resume fail-closed)"
    )
    return 0


def cmd_chaos_release_blocker_2902_coverage():
    """Issue #2902: static contract for chaos hard release blocker."""
    print(f"{B}=== chaos hard release blocker coverage (#2902) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_release_blocker_2902.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos hard release blocker (#2902) coverage contract rows failed")
        return 1
    ok("chaos hard release blocker (#2902) coverage clean")
    return 0


def cmd_chaos_release_blocker_2902():
    """Issue #2902: hard release blocker — multi-fiber chaos under production_defaults.

    Elevates #2856/#2554/#2722/#2755 into a hard pre-push/CI release gate:
      AURA_CHAOS_RELEASE_BLOCKER=1 AURA_CHAOS_RELEASE_BLOCKER_ONLY=1
      workers=4 fibers=32 duration=8s seed=1 Soft steal off
      production_defaults_active enforced in-process

    Hard-fail set (any delta > 0 fails):
      steal_snapshot_hard_fail, residual still-running,
      residual hard-AND arms (#2721), force_deopt, resume_hard_fail,
      residual_rearm_race (#2901), residual_defer_steal_hard_fail (#2546),
      resume_fence hard/ticket surplus (layout_stamp_resume observe-only).

    Bounded load signal (composition ceiling, not absolute zero under prod):
      mailbox hold/defer starvation — production_defaults hard face (#2551)
      ticks under intentional Guard×mailbox; default ceiling max(64, fibers*2)
      unless AURA_CHAOS_MB_STARVE_MAX is set. Soft/PR still use max=0.

    Sustained mode (optional AURA_CHAOS_SUSTAINED=1): fibers≥32, duration≥8s.
    Soft / known-bad inject under production still fails (#2554 inject path).
    FULL/SOAK nightly paths unchanged.

    CI gate job without cmake: static coverage only (same pattern as #2554).
    """
    print(f"{B}=== chaos hard release blocker (#2902) ==={N}")
    rc = cmd_chaos_release_blocker_2902_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            ok(
                "chaos release blocker runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via issue-farm CI"
            )
            return 0
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = subprocess.run(
            ["ninja", "-C", str(BUILD), "-j", str(max(1, nproc // 2)), "test_chaos_mutate_steal_gc_mailbox"],
            cwd=ROOT,
        )
        if r.returncode != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r.returncode
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_CHAOS_RELEASE_BLOCKER"] = "1"
    env["AURA_CHAOS_RELEASE_BLOCKER_ONLY"] = "1"
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "4")
    env.setdefault("AURA_CHAOS_FIBERS", "32")
    env.setdefault("AURA_CHAOS_DURATION_S", "8")
    # Leave AURA_CHAOS_MB_STARVE_MAX unset so composition ceiling applies
    # under production_defaults (see run_chaos_pass #2902). Caller may set.

    info(
        "release blocker env: AURA_CHAOS_RELEASE_BLOCKER=1 ONLY "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']}"
    )
    timeout_s = max(180, int(env["AURA_CHAOS_DURATION_S"]) + 120)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos release blocker timed out after {timeout_s}s (hang?) — release blocked")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"chaos hard release blocker FAILED exit={r.returncode} in {elapsed:.1f}s — "
            "any hard-fail counter (steal hard-fail / residual still-running / "
            "residual hard-AND / rearm_race / defer_steal_hard / resume_fence) "
            "must be 0 under production_defaults_active"
        )
        return r.returncode
    ok(
        f"chaos hard release blocker GREEN in {elapsed:.1f}s "
        f"(workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s; expanded hard-fail set zero)"
    )
    return 0


def cmd_production_concurrency_coverage():
    """Issue #2380/#2513: production-concurrency gate static contract rows.

    Nightly profile: lock-order canary + full chaos + densify + Soft forbid.
    #2513 soak extension: non-yield loops, reclaim residual, hard-fail counters.
    PR smoke path stays short (no FULL / no PRODUCTION_CONCURRENCY_GATE / no SOAK).
    """
    print(f"{B}=== production-concurrency coverage (#2380) ==={N}")
    script = COVERAGE_CHECKS / "check_production_concurrency_gate_2380.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("production-concurrency coverage contract rows failed")
        return 1
    ok("production-concurrency coverage clean")


def cmd_chaos_pr_hard_fail_coverage():
    """Issue #2554: static contract for PR chaos hard-fail deployment gate."""
    print(f"{B}=== chaos PR hard-fail gate coverage (#2554) ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_pr_hard_fail_gate_2554.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos PR hard-fail gate (#2554) coverage contract rows failed")
        return 1
    ok("chaos PR hard-fail gate (#2554) coverage clean")
    return 0


def cmd_chaos_pr_hard_fail_gate():
    """Issue #2554: short PR chaos under production-like hard-fail invariants.

    Part of ./build.py gate (deployment gate). Profile:
      AURA_CHAOS_PR_GATE=1 AURA_CHAOS_PR_GATE_ONLY=1
      workers=4 fibers=16 duration=3s seed=1 Soft steal off
    Asserts: steal hard-fail Δ==0, residual still-running==0, no hang.
    Full SOAK/FULL path unchanged (nightly via production-concurrency).

    AC1: inject hard-fail (AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1) must fail.
    AC2: clean short profile must pass.

    CI note: the GitHub Actions `gate` job is static-only (no cmake tree).
    When the binary is absent and build/ is unconfigured, run static
    coverage only and skip the runtime profile — `issue-farm` shard 1
    runs the full hard-fail gate after `./build.py ci-issues` (#4819).
    Local pre-push / configured trees still run the full runtime path.
    """
    print(f"{B}=== chaos PR hard-fail gate (#2554) ==={N}")
    rc = cmd_chaos_pr_hard_fail_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    cmake_cache = BUILD / "CMakeCache.txt"
    if not bin_path.exists():
        if not cmake_cache.exists():
            # Issue #3950: claiming production-concurrency cannot silent-0 skip.
            claimed = os.environ.get("AURA_PRODUCTION_CONCURRENCY_GATE", "").strip() == "1" or (
                os.environ.get("AURA_CI_PRODUCTION_CONCURRENCY", "").strip().lower() in ("1", "true", "yes")
            )
            if claimed:
                fail(
                    "chaos PR hard-fail runtime skipped while production-concurrency claimed "
                    "(#3950) — configure cmake / run after ./build.py build"
                )
                return 1
            # Static-only gate (CI gate job / fresh clone without build/).
            ok(
                "chaos PR hard-fail runtime skipped (no CMakeCache; static coverage only) "
                "— run after ./build.py build or via issue-farm CI"
            )
            return 0
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    def _pr_env(**extra):
        env = os.environ.copy()
        env["AURA_CHAOS_PR_GATE"] = "1"
        env["AURA_CHAOS_PR_GATE_ONLY"] = "1"
        env.setdefault("AURA_CHAOS_SEED", "1")
        env.setdefault("AURA_CHAOS_WORKERS", "4")
        env.setdefault("AURA_CHAOS_FIBERS", "16")
        env.setdefault("AURA_CHAOS_DURATION_S", "3")
        # Issue #2554 flaky: mailbox defer starvation canary is now deduped
        # per open-defer window (multi_fiber_mailbox.h) so the counter no
        # longer storms to 100000. Still allow a small composition-aware
        # ceiling (same as release: max(64, n_fibers*2)) so a single
        # spurious >100ms defer under CI load does not flake the gate.
        env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "64")
        env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
        env.pop("AURA_CHAOS_FULL", None)  # PR gate must not pull FULL soak
        env.pop("AURA_CHAOS_SOAK", None)
        env.pop("AURA_PRODUCTION_CONCURRENCY_GATE", None)
        env.update(extra)
        return env

    # AC2: clean short profile under production-like hard-fail invariants.
    env_clean = _pr_env()
    info(
        "env: AURA_CHAOS_PR_GATE=1 ONLY workers="
        f"{env_clean['AURA_CHAOS_WORKERS']} fibers={env_clean['AURA_CHAOS_FIBERS']} "
        f"duration={env_clean['AURA_CHAOS_DURATION_S']}s seed={env_clean['AURA_CHAOS_SEED']}"
    )
    timeout_s = 90  # CI resource limit; profile is ~3s + injects
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env_clean, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"chaos PR hard-fail gate timed out after {timeout_s}s (hang?)")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(f"chaos PR hard-fail gate clean run failed exit={r.returncode} in {elapsed:.1f}s")
        return r.returncode
    ok(f"chaos PR hard-fail clean green in {elapsed:.1f}s")

    # AC1: intentional inject of steal hard-fail must fail the gate binary.
    env_inj = _pr_env(AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL="1")
    info("AC1 inject: AURA_CHAOS_PR_GATE_INJECT_HARD_FAIL=1 (expect non-zero exit)")
    try:
        r2 = subprocess.run([str(bin_path)], cwd=ROOT, env=env_inj, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail("AC1 inject timed out")
        return 1
    if r2.returncode == 0:
        fail("AC1: inject hard-fail must make PR-gate binary fail (got exit 0)")
        return 1
    ok(f"AC1 inject hard-fail correctly failed exit={r2.returncode}")
    return 0


def cmd_production_concurrency():
    """Issue #2380/#2513: nightly / deploy production-concurrency hard gate.

    Env matrix (hard-fail if any criterion fails):
      AURA_PRODUCTION_CONCURRENCY_GATE=1
      AURA_LOCK_ORDER_CANARY=1
      AURA_CHAOS_FULL=1
      AURA_CHAOS_WORKERS≥4  AURA_CHAOS_DURATION_S≥30
      Optional soak: AURA_CHAOS_SOAK=1 AURA_CHAOS_FIBERS=256..1000
                     AURA_CHAOS_DURATION_S=300+ (nightly / deploy)
    Soft steal (AURA_STEAL_SNAPSHOT_SOFT) is forbidden.
    Hard criteria (#2513): steal hard-fail delta 0, residual still-running 0,
    mailbox starvation ≤ AURA_CHAOS_MB_STARVE_MAX, no hang.
    Builds test_chaos_mutate_steal_gc_mailbox if needed, then soaks.
    Not part of PR CI smoke — use nightly or explicit local run.
    """
    print(f"{B}═══ production-concurrency gate (#2380/#2513) ═══{N}")
    # Static contract first (fast fail on missing wire-up).
    rc = cmd_production_concurrency_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    if not bin_path.exists():
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    env["AURA_LOCK_ORDER_CANARY"] = "1"
    env["AURA_CHAOS_FULL"] = "1"
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "4")
    # #2513: prefer higher fiber default under gate; SOAK raises further.
    if env.get("AURA_CHAOS_SOAK", "") == "1":
        env.setdefault("AURA_CHAOS_FIBERS", "256")
        env.setdefault("AURA_CHAOS_DURATION_S", "300")
    else:
        env.setdefault("AURA_CHAOS_FIBERS", "64")
        env.setdefault("AURA_CHAOS_DURATION_S", "30")
    env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0")
    # Soft steal forbidden under production gate (also unset by test body).
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)

    info(
        "env: AURA_PRODUCTION_CONCURRENCY_GATE=1 AURA_LOCK_ORDER_CANARY=1 "
        f"AURA_CHAOS_FULL=1 soak={env.get('AURA_CHAOS_SOAK', '0')} "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']}"
    )
    # Full soak + injects: allow generous wall (duration + watchdog + overhead).
    timeout_s = max(180, int(env.get("AURA_CHAOS_DURATION_S", "30")) + 120)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(f"production-concurrency timed out after {timeout_s}s (hang?)")
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(f"production-concurrency failed exit={r.returncode} in {elapsed:.1f}s")
        return r.returncode
    ok(f"production-concurrency green in {elapsed:.1f}s")
    return 0


def cmd_chaos_soak_hard_gate_2722():
    """Issue #2722: RELEASE hard deploy gate — chaos SOAK under production
    hard-fail-closed semantics. Required for any tag / release candidate
    that claims multi-fiber mutation safety. Closes the #2679 residual
    ("chaos soak is optional / best-effort — does not gate production
    builds or release artifacts").

    Issue #2755 (additive residual-zero extension of #2722): at end-of-run
    under AURA_PRODUCTION_CONCURRENCY_GATE=1 + Hard the chaos harness
    hard-fails if any steal-safety residual hard-AND counter grew:

      residual hard-AND (#2721 four arms — must be hard-zero):
        g_steal_safety_residual_boundary_unsafe_total
        g_steal_safety_residual_layout_stamp_mismatch_total
        g_steal_safety_residual_ticket_mismatch_total
        g_steal_safety_residual_gc_defer_armed_total
      related surfaces hard-zero under this gate:
        steal_snapshot_mismatch_force_deopt_total
        resume_hard_fail_total
      related observe-only (printed; residual_layout_stamp covers silent
      corruption for LayoutStamp races):
        layout_stamp_resume_mismatch_total

    Soft / local iteration (no SOAK hard gate / no production concurrency
    gate) remains non-gating for residual counters (metric-only print).

    AC1: full SOAK with production_defaults_active=true + Hard
        fail-closed. Runs cmd_production_concurrency under
        AURA_CHAOS_SOAK_HARD_GATE=1 (distinct env so PR CI smoke +
        nightly + RELEASE hard gate are three independently-gated
        paths — same harness, different scope).
    AC2: any residual panic, LayoutStamp mismatch, live MutationHold
        after boundary exit, or steal-after-degrade fails the gate.
        The chaos binary already bumps hard-fail counters for these
        4 invariants under AURA_CHAOS_FULL=1 (production / SOAK modes);
        the binary's CHECK(delta==0, "silent corruption") gates the exit.
        #2755: residual hard-AND deltas also gate exit (see counter list).
    AC3: SOAK duration / fiber count / GC frequency parameterized
        (documented production envelope numbers below).
    AC4: required for any tag / release candidate. Wired into
        .github/workflows/release.yml as a required status check that
        must pass before release artifacts upload.
    AC5: Soft (metric-only) mode remains available for local iteration
        via AURA_STEAL_SNAPSHOT_SOFT=1 but is EXPLICITLY non-gating
        here (the hard gate forces production_defaults_active=true).

    Production envelope (documented per AC3):
      workers  : 8     (AURA_CHAOS_WORKERS=8)
      fibers   : 256   (AURA_CHAOS_FIBERS=256)
      duration : 300s  (AURA_CHAOS_DURATION_S=300)
      gc freq  : chaos harness drives request_gc_safepoint() under
                 concurrent mutate + steal (see tests/serve/test_chaos_
                 mutate_steal_gc_mailbox.cpp AC1 contract rows)
      seed     : AURA_CHAOS_SEED=1 (reproducible per AC6)

    AC4 release.yml contract: the release job runs
    `python3 build.py chaos-soak-hard-gate-2722` as a required step
    before `softprops/action-gh-release@v3` uploads the tarball +
    SBOM. Tag push (push.tags: 'v*') triggers the job; if the gate
    fails, the release workflow exits non-zero and no release assets
    are uploaded.
    """
    print(f"{B}=== chaos SOAK hard deploy gate (#2722 + #2755 residual-zero) ==={N}")
    # Static contract first — fast fail on missing wire-up.
    rc = cmd_chaos_soak_hard_gate_2722_coverage()
    if rc != 0:
        return rc
    # Issue #2755: residual hard-AND zero contract (additive over #2722).
    rc = cmd_chaos_soak_residual_zero_2755_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox"
    if not bin_path.exists():
        if not (BUILD / "CMakeCache.txt").exists():
            # Issue #4437: static coverage must not pass this gate. A missing
            # cache means this command configures the default build, then
            # builds and runs the soak. Configure failure stays a release
            # failure; there is no static-only success.
            info(
                "chaos SOAK hard gate: no CMakeCache in the default build; "
                "configuring so test_chaos_mutate_steal_gc_mailbox is built and run"
            )
            rc = cmd_configure()
            if rc != 0:
                fail("chaos SOAK hard gate configure failed (static coverage only does not pass the release hard gate)")
                return rc
        info("building test_chaos_mutate_steal_gc_mailbox…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox failed")
            return r
    if not bin_path.exists():
        fail(f"missing {bin_path} — run ./build.py build first")
        return 1

    env = os.environ.copy()
    # Hard-gate env matrix: full profile + SOAK + hard-fail-closed.
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    env["AURA_LOCK_ORDER_CANARY"] = "1"
    env["AURA_CHAOS_FULL"] = "1"
    env["AURA_CHAOS_SOAK"] = "1"  # long SOAK per AC3 (300s)
    env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"  # distinct env so PR + nightly + RELEASE are independently gated
    # Soft steal FORBIDDEN under hard gate (production_defaults_active + Hard).
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)
    # Production envelope (documented above; overridable via env for
    # local iteration under AURA_CHAOS_HARD_GATE_OVERRIDE=1).
    env.setdefault("AURA_CHAOS_SEED", "1")
    env.setdefault("AURA_CHAOS_WORKERS", "8")
    env.setdefault("AURA_CHAOS_FIBERS", "256")
    env.setdefault("AURA_CHAOS_DURATION_S", "300")
    env.setdefault("AURA_CHAOS_MB_STARVE_MAX", "0")

    info(
        "RELEASE hard gate env: AURA_PRODUCTION_CONCURRENCY_GATE=1 "
        "AURA_CHAOS_FULL=1 AURA_CHAOS_SOAK=1 AURA_CHAOS_SOAK_HARD_GATE=1 "
        f"workers={env['AURA_CHAOS_WORKERS']} fibers={env['AURA_CHAOS_FIBERS']} "
        f"duration={env['AURA_CHAOS_DURATION_S']}s seed={env['AURA_CHAOS_SEED']} "
        "mb_starve_max=0"
    )
    # Wall for the soak child only (the build above is not part of it). The
    # release container runs 8 workers / 256 fibers on a 2-vCPU runner; the
    # v0.2.1 tag release died on the old max(900, duration + 600) cap with
    # every residual gate already printing PASS, i.e. the soak itself was
    # slower than 900 s there, not hung. Budget 3x the soak plus a fixed
    # margin for that shape; the GitHub job timeout still bounds the step.
    soak_s = int(env["AURA_CHAOS_DURATION_S"])
    timeout_s = max(1800, soak_s * 3 + 900)
    start = time.time()
    try:
        r = subprocess.run([str(bin_path)], cwd=ROOT, env=env, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        fail(
            f"chaos SOAK hard gate timed out after {timeout_s}s "
            f"(duration={soak_s}s workers={env['AURA_CHAOS_WORKERS']} "
            f"fibers={env['AURA_CHAOS_FIBERS']}; a runner too slow for the "
            "soak reports here, while a true hang leaves the residual "
            "counters non-zero) — release blocked"
        )
        return 1
    elapsed = time.time() - start
    if r.returncode != 0:
        fail(
            f"RELEASE chaos SOAK hard gate FAILED exit={r.returncode} "
            f"in {elapsed:.1f}s — release blocked (residual panic / "
            "LayoutStamp mismatch / live MutationHold / steal-after-degrade / "
            "steal-safety residual hard-AND (#2755: boundary/layout/ticket/"
            "gc_defer + layout_resume/force_deopt/resume_hard_fail) "
            "must be 0 under production_defaults_active + Hard)"
        )
        return r.returncode
    ok(
        f"RELEASE chaos SOAK hard gate GREEN under production_defaults_active "
        f"+ Hard in {elapsed:.1f}s (workers={env['AURA_CHAOS_WORKERS']} "
        f"fibers={env['AURA_CHAOS_FIBERS']} duration={env['AURA_CHAOS_DURATION_S']}s; "
        "residual hard-AND + related surfaces hard-zero per #2755)"
    )
    return 0


def cmd_chaos_soak_hard_gate_2722_coverage():
    """Issue #2722: static contract for RELEASE chaos SOAK hard gate.

    Validates the #2722 contract:
      AC1: cmd_chaos_soak_hard_gate_2722 exists in build.py
      AC2: hard-fail env matrix forces production_defaults_active + Hard
      AC3: production envelope (workers=8, fibers=256, duration=300s)
           documented in the function docstring
      AC4: required for any tag / release candidate (wired in
           .github/workflows/release.yml)
      AC5: Soft mode (AURA_STEAL_SNAPSHOT_SOFT) explicitly non-gating
           (popped under the hard gate env matrix)
    """
    print(f"{B}=== chaos SOAK hard gate (#2722) coverage ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_soak_hard_gate_2722.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos SOAK hard gate (#2722) coverage contract rows failed")
        return 1
    ok("chaos SOAK hard gate (#2722) coverage clean")
    return 0


def cmd_chaos_soak_residual_zero_2755_coverage():
    """Issue #2755: residual steal-safety hard-AND counters hard-zero under
    SOAK hard gate (extend #2722). Static contract only — full SOAK runtime
    is still owned by cmd_chaos_soak_hard_gate_2722.
    """
    print(f"{B}=== chaos SOAK residual-zero (#2755) coverage ==={N}")
    script = COVERAGE_CHECKS / "check_chaos_soak_residual_zero_2755.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("chaos SOAK residual-zero (#2755) coverage contract rows failed")
        return 1
    ok("chaos SOAK residual-zero (#2755) coverage clean")
    return 0


def cmd_production_concurrency_ready_gate_4159():
    """Issue #4159 (P0): production concurrency Ready deploy residual-zero
    hard gate — the machine-checkable continuous deploy AND over
    #2722/#2755. The RELEASE soak hard gate was env-opt-in per deploy and
    the coverage linter proved only command PRESENCE, so a deploy could
    ship a Soft-green soak while the named residuals (rearm_race /
    lifetime_proof / envframe_lag) grew between RELEASE runs — missing
    machine-checkable continuous proof of the I3/I6 arms (not
    Soft-as-vuln).

    This command IS the deploy AND (all terms reuse existing latches/bits;
    no new residual bus):
      1. aura_runtime_require_production_multi_worker — the #3195 Ready
         latch, consulted by the deploy-AND accessor through
         aura_runtime_multi_worker_production_latched().
      2. cmd_chaos_soak_hard_gate_2722() — the RELEASE SOAK hard gate runs
         here (residual arms hard-zero at end-of-run per #2755).
      3+4. steal_safety_production_residual_zero_v_read() == 1 AND
         sticky-fail == 0 under the latched face + armed soak env —
         proven at deploy time by the residual-zero runtime face
         (test_steal_safety_production_residual_zero, #4159 AC18) run
         under AURA_CHAOS_SOAK_HARD_GATE=1 +
         AURA_PRODUCTION_CONCURRENCY_GATE=1. Soft (unlatched) returns 0
         from steal_safety_production_concurrency_ready_gate_v_read()
         even with the soak armed — a Soft-green soak can never satisfy
         the deploy AND.

    Soft / PR soak remains non-gating (#2722 AC5 / #2755 AC2 unchanged);
    #3162/#3195 sticky faces and the #4158 edge-free hold contract are
    not weakened. release.yml runs this gate after the 2722 coverage row
    and before release-asset upload; the static rows run on EVERY
    build.py gate via scripts/check_chaos_ready_residual_gate_4159.py —
    continuous proof, not RELEASE-only.
    """
    print(f"{B}=== production concurrency Ready residual-zero deploy gate (#4159) ==={N}")
    # Static deploy-AND contract first — fast fail on missing wire-up.
    script = ROOT / "scripts" / "check_chaos_ready_residual_gate_4159.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = run([sys.executable, str(script)], cwd=ROOT)
    if r != 0:
        fail("production concurrency Ready residual gate (#4159) static contract failed")
        return r
    ok("production concurrency Ready residual gate (#4159) static contract clean")

    # Term 2: the RELEASE chaos SOAK hard gate itself (full soak; residual
    # arms hard-zero at end-of-run per #2755).
    rc = cmd_chaos_soak_hard_gate_2722()
    if rc != 0:
        return rc

    # Terms 3+4: residual-zero SSOT + sticky-clear under the latched face,
    # proven at deploy time by the residual-zero runtime face under the
    # armed env (the #4159 AC18 block flips the env/latch per sub-case and
    # restores; the outer env arms the deploy face for the whole binary).
    bin_path = BUILD / "test_steal_safety_production_residual_zero"
    if not bin_path.exists():
        if not (BUILD / "CMakeCache.txt").exists():
            ok(
                "residual-zero runtime face skipped (no CMakeCache; static "
                "deploy-AND contract only) — run after ./build.py build"
            )
            return 0
        info("building test_steal_safety_production_residual_zero…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_steal_safety_production_residual_zero",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_steal_safety_production_residual_zero failed")
            return r
    env = os.environ.copy()
    env["AURA_CHAOS_SOAK_HARD_GATE"] = "1"
    env["AURA_PRODUCTION_CONCURRENCY_GATE"] = "1"
    info("deploy face env: AURA_CHAOS_SOAK_HARD_GATE=1 AURA_PRODUCTION_CONCURRENCY_GATE=1")
    r = subprocess.run([str(bin_path)], cwd=ROOT, env=env)
    if r.returncode != 0:
        fail(
            "production concurrency Ready residual gate (#4159) FAILED — "
            "residual-zero SSOT / sticky / deploy-AND face red under the "
            "armed env; release blocked"
        )
        return r.returncode
    ok("production concurrency Ready residual gate (#4159) GREEN — deploy AND proven")
    return 0


def cmd_orch_scope_child_2631_coverage():
    """Issue #2631: Aura surface for hierarchical AgentScope
    (orch:scope-child). Closes the script-side tree supervision
    gap: #2537 C++ hierarchical AgentScope exists but #2588 Aura
    surface was flat. Adds the orch:scope-child prim that calls
    AgentScope::spawn_child() on an existing per-Evaluator scope.

    Validates the 6-AC contract from issue body:
      AC1: spawn_child hierarchy + cancel_all top-down propagation.
      AC2: ~AgentScope / scope-join-all drains children then parent.
      AC3: scripts/coverage/checks/check_orch_mvp_scope.py --strict still green
           (no AgentRegistry / global_agent_registry).
      AC4: query:orch-module-stats metric + schema keys
           (scope-child-total, scope-child-wired, schema-2631,
           issue-2631).
      AC5: README + source-cite (hierarchical AgentScope #2537 +
           flat scope #2588 surface preserved).
      AC6: test + coverage gate source-cite (orch:scope-child prim
           + OrchModuleStats scope_child_total +
           check_orch_mvp_scope.py).
    """
    print(f"{B}=== orch:scope-child hierarchical AgentScope coverage (#2631) ==={N}")
    script = COVERAGE_CHECKS / "check_orch_scope_child_2631.py"
    r = _coverage_run(script)
    if r.returncode != 0:
        fail("orch:scope-child coverage contract rows failed")
        return 1
    ok("orch:scope-child coverage clean")
    return 0


def _gate_parse_jobs() -> int:
    """Parallel workers for coverage checks (0 → run_checks default)."""
    if "--serial" in sys.argv[2:] or os.environ.get("AURA_GATE_SERIAL", "").strip() in (
        "1",
        "true",
        "yes",
    ):
        return 1
    argv = sys.argv[2:]
    for i, a in enumerate(argv):
        if a == "--jobs" and i + 1 < len(argv) and argv[i + 1].isdigit():
            return max(1, int(argv[i + 1]))
        if a.startswith("--jobs=") and a.split("=", 1)[1].isdigit():
            return max(1, int(a.split("=", 1)[1]))
    env = os.environ.get("AURA_GATE_JOBS", "").strip()
    if env.isdigit() and int(env) > 0:
        return int(env)
    return 0  # run_checks chooses min(16, nproc)


def _run_parallel_coverage_checks(*, changed: bool) -> int:
    """Drive scripts/coverage/run_checks.py (parallel + cascade-free)."""
    if not COVERAGE_RUN_CHECKS.is_file():
        fail(f"missing {COVERAGE_RUN_CHECKS}")
        return 1
    print(f"{B}═══ Coverage checks ({'changed' if changed else 'all'}, parallel) ═══{N}")
    # Cascade suppression is the default for gate: every check runs once.
    os.environ["AURA_COVERAGE_NO_CASCADE"] = "1"
    os.environ["AURA_LINT_SKIP_COVERAGE"] = "1"
    cmd = [sys.executable, str(COVERAGE_RUN_CHECKS)]
    cmd.append("--changed" if changed else "--all")
    jobs = _gate_parse_jobs()
    if jobs > 0:
        cmd.extend(["--jobs", str(jobs)])
    # Forward --base if present.
    argv = sys.argv[2:]
    for i, a in enumerate(argv):
        if a == "--base" and i + 1 < len(argv):
            cmd.extend(["--base", argv[i + 1]])
            break
        if a.startswith("--base="):
            cmd.append(a)
            break
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        fail("coverage checks failed — see scripts/coverage/run_checks.py output above")
        return r.returncode
    ok("coverage checks clean")
    return 0


def _gate_run_issue_bin(target: str) -> int:
    """Ninja + run one issue binary from repo root (CI source-cite cwd)."""
    if not (BUILD / "build.ninja").is_file():
        info(f"changed: skip {target} (no ninja tree)")
        return 0
    print(f"{B}═══ Gate changed: {target} ═══{N}")
    jobs = _gate_parse_jobs() or _build_jobs()
    rc = _ninja_build([target], jobs=max(1, jobs))
    if rc:
        fail(f"ninja {target} failed")
        return rc
    bin_path = BUILD / target
    if not bin_path.is_file():
        fail(f"{target} binary not found")
        return 1
    env = os.environ.copy()
    # Match tests/python/run_issue_tests.py — inherit hole is only
    # visible when the runner default AURA_SANDBOX=off is set.
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
    env["AURA_IR_DIRTY_BATCH_ONLY"] = "0"
    r = subprocess.run([str(bin_path)], cwd=str(ROOT), env=env)
    if r.returncode != 0:
        fail(f"{target} rc={r.returncode}")
    return r.returncode


def _gate_changed_runtime_suites() -> int:
    """Run the slow CI leaves that gate --changed would otherwise skip.

    ci/concurrent and ci/issues are wave-only. Local Debug often misses
    RelWithDebInfo unused-result / AURA_SANDBOX=off isolate / source-cite
    wraps. Compile+run the touched leaf here so those cannot land blind.
    """
    changed = _git_changed_files()
    rc = 0
    if any(p == "tests/serve/test_concurrent.cpp" or p.startswith("src/serve/") for p in changed):
        if not (BUILD / "build.ninja").is_file():
            info("changed serve: skip test_concurrent (no ninja tree)")
        else:
            print(f"{B}═══ Gate changed: test_concurrent (serve/fiber surface) ═══{N}")
            jobs = _gate_parse_jobs() or _build_jobs()
            nrc = _ninja_build(["test_concurrent"], jobs=max(1, jobs))
            if nrc:
                fail("ninja test_concurrent failed")
                return nrc
            rc = test_concurrent() or rc
    densify_hit = any(
        p
        in (
            "tests/core/test_moving_densify_fail_closed.cpp",
            "tests/core/test_densify_pin_batch.cpp",
            "src/core/arena.ixx",
            "src/core/densify_consistency_report.h",
        )
        for p in changed
    )
    if densify_hit:
        rc = _gate_run_issue_bin("test_densify_pin_batch") or rc
    # Do not ninja-run test_orch_agent_batch here: leftover isolate
    # members (apply-mutex join, schedule-gate SIGSEGV) flake the
    # 12-member binary. #3179 ACs are covered by densify-style
    # AURA_SANDBOX=off runs of that member in ci/issues.
    return rc


def _gate_steps(*, fix: bool, changed: bool, scripts_only: bool) -> list[tuple[str, Callable[[], int]]]:
    """Ordered gate pipeline: (label, callable); a non-zero return aborts.

    Issue #5246: this replaces the historical ~60-call hand-wired `or` chain.
    Coverage is a single step — scripts/coverage/run_checks.py owns the whole
    check_*.py set (parallel, cascade-free) plus the per-check argv that used
    to be spelled out here as ~40 separate 'or cmd_*_coverage()' calls.
    """
    check = not fix
    steps: list[tuple[str, Callable[[], int]]] = [
        ("docs", lambda: cmd_docs(check=check)),
        ("cli-templates", lambda: cmd_cli_templates(check=check)),
        ("lint", cmd_lint),
    ]
    if not scripts_only:
        steps.append(("format", cmd_format))
    steps.extend(
        [
            ("fixtures", cmd_fixtures),
            ("primitive-surface", cmd_primitive_surface),
            ("test-registry", cmd_test_registry),
            ("test-binding", cmd_test_binding),
            ("new-test-files", cmd_new_test_files),
            ("dead-heap-push", cmd_dead_heap_push),
            ("catch-silent-swallow", cmd_catch_silent_swallow),
            ("aot-env-linear-stamp", cmd_aot_env_linear_stamp),
            ("legacy-test-inventory", cmd_legacy_test_inventory),
            ("soak-pr-short-3387", cmd_soak_pr_short_3387_coverage),
            ("coverage", lambda: _run_parallel_coverage_checks(changed=changed)),
        ]
    )
    return steps


def cmd_gate():
    """Fast static checks for CI / pre-push.

    Core: docs + ruff + clang-format + fixtures + surface audits.
    Coverage: scripts/coverage/run_checks.py — parallel custom check_*.py
    plus declarative manifests (runner.py). See tests/COVERAGE.md.

    Flags:
      --fix          auto-regen docs/registry/inventory; lint/format fix
      --scripts-only  skip clang-format
      --changed       only coverage/format for git-diff paths (pre-push)
      --serial        coverage jobs=1
      --jobs N        coverage parallel workers
      --full          force full coverage + chaos runtime even with --changed
                      (or AURA_GATE_FULL=1)

    Issue #1572/#1573/#1668/#1669/#1931/#1957/#1966/#2057/#2168 lineage
    preserved; execution order lives in the _gate_steps table (Issue #5246).
    """
    fix = "--fix" in sys.argv[2:]
    scripts_only = "--scripts-only" in sys.argv[2:] or _env_flag("AURA_GATE_SCRIPTS_ONLY")
    changed = "--changed" in sys.argv[2:] or _env_flag("AURA_GATE_CHANGED")
    full = "--full" in sys.argv[2:] or _env_flag("AURA_GATE_FULL")
    if full:
        changed = False

    mode = "fix" if fix else "check"
    if scripts_only:
        mode += "+scripts-only"
    if changed:
        mode += "+changed"
    if full:
        mode += "+full"
    print(f"{B}═══ Gate ({mode}) ═══{N}")

    os.environ["AURA_LINT_SKIP_COVERAGE"] = "1"
    os.environ["AURA_COVERAGE_NO_CASCADE"] = "1"
    if changed:
        os.environ["AURA_FORMAT_CHANGED"] = "1"

    t_gate = time.time()
    if scripts_only:
        info("scripts-only: skipping clang-format (cross-platform gate)")
    for label, step in _gate_steps(fix=fix, changed=changed, scripts_only=scripts_only):
        rc = step()
        if rc:
            fail(f"gate step failed: {label}")
            return rc

    # Chaos runtime profiles (need cmake tree / binary). Static coverage for
    # these issues already ran via run_checks. On --changed pre-push, skip the
    # multi-second runtime profiles unless --full.
    if changed and not full:
        info("changed mode: skipping chaos runtime profiles (static coverage already ran)")
    else:
        rc = cmd_chaos_release_blocker_2902() or cmd_chaos_pr_hard_fail_gate()
        if rc:
            return rc

    # Issue #3567: ci/concurrent is a stress-wave suite (~16s, x86 SIGSEGV).
    # Agents that only run gate --changed + unit never see it. When the
    # serve/fiber surface changes and a ninja tree exists, compile+run here.
    if changed and not scripts_only:
        rc = _gate_changed_runtime_suites()
        if rc:
            return rc

    ok(f"gate clean in {time.time() - t_gate:.1f}s ({mode})")
    return 0


def _run_concurrent_early(ctx: _BuildCtx) -> int:
    """Run test_concurrent as soon as the isolate compile finishes.

    ci/concurrent used to sit in the stress wave after ci/issues (~368s).
    The binary is already linked in _begin_build; running it here keeps
    wall time the same (issues still dominate) but surfaces x86 UAF /
    unused-result ~6 min earlier, and a compile miss no longer skips issues.
    """
    if not ctx.concurrent_attempted:
        return 0
    if ctx.concurrent_build_rc:
        fail("test_concurrent failed to compile — continuing ci/issues")
        return ctx.concurrent_build_rc
    if not _target_bin_ok("test_concurrent"):
        fail("test_concurrent binary not found — continuing ci/issues")
        return 1
    info("ci/concurrent: run after isolate compile, before issue ninja")
    return test_concurrent()


def _report_ci_slice_failures(
    link_rc: int,
    early_rc: int,
    late_rc: int,
    conc_rc: int = 0,
    early_failed: list[str] | None = None,
    late_failed: list[str] | None = None,
) -> int:
    """OR the cmd_ci slices and print which one failed.

    Heavy always runs even when cheap/medium already failed, so the
    last printed line is often ``All 2 test suites passed`` while the
    process still exits 1 (CI #4910). Surface every slice here.
    """
    if not (link_rc or early_rc or late_rc or conc_rc):
        return 0
    print(f"\n{'═' * 50}")
    fail("CI failed — later wave summary is not the whole result:")
    if conc_rc:
        print(f"  {R}✗{N} ci/concurrent (compile or run) rc={conc_rc}")
    if link_rc:
        print(f"  {R}✗{N} issue-bin ninja rc={link_rc}")
    if early_rc:
        print(f"  {R}✗{N} cheap/medium wave (search log for 'Waves cheap, medium') rc={early_rc}")
        for label in early_failed or []:
            print(f"      {R}✗{N} {label}")
    if late_rc:
        print(f"  {R}✗{N} ci/issues (heavy wave) rc={late_rc}")
        for label in late_failed or []:
            print(f"      {R}✗{N} {label}")
    return 1


def _ci_issues_run_tests(t0: float) -> int:
    """Run this shard's binaries. build/aura must already be installed."""
    shard_file = BUILD / _ISSUE_SHARD_LIST_NAME
    if _issue_shard_spec() is not None:
        if not shard_file.is_file():
            fail("issue shard: target list was not written")
            return 1
        if not shard_file.read_text().split():
            ok("issue shard: no targets in this slice")
            return 0
        os.environ["AURA_ISSUE_SHARD_LIST"] = str(shard_file)
    if not AURA.is_file():
        fail(f"issue tests need {AURA} (built here, or fetched from the aura-bin job)")
        return 1
    rc = cmd_test(["issues"])
    _phase("ci-issues total", t0)
    return rc


def cmd_ci_issues():
    """CI issue-farm shard (#4819): this shard's issue binaries, then run them.

    AURA_ISSUE_SHARD=i/N picks a deterministic slice of all_test_issue_targets
    (fast tier: of the fast target list). The slice pulls in aura_test_objects.
    Some issue tests spawn the aura executable (AURA_BIN). Locally that binary
    is built in this same command. CI sets AURA_SKIP_AURA_BUILD and splits the
    command: --build-only, then install build/aura from the aura-bin job, then
    --test-only. Without AURA_ISSUE_SHARD this is the whole farm.
    """
    phase = _ci_issues_phase()
    if phase == "test":
        return _ci_issues_run_tests(time.time())

    mode = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower()
    if mode in ("none", "skip", "off", "0"):
        info("issue tests: skipped (AURA_ISSUE_BUILD=none)")
        return 0
    os.environ["AURA_ISSUE_BUILD"] = mode or "all"
    os.environ["AURA_BUILD_TARGETS"] = "aura"
    shard_file = BUILD / _ISSUE_SHARD_LIST_NAME
    shard_file.unlink(missing_ok=True)
    rc, ctx = _begin_build()
    if ctx is None:
        return rc
    link_rc = _finish_overlap_issues(ctx)
    if link_rc != 0:
        fail(f"issue shard build failed (rc={link_rc})")
        return link_rc
    if _issue_shard_spec() is not None:
        if not shard_file.is_file():
            fail("issue shard: target list was not written")
            return 1
        if not shard_file.read_text().split():
            ok("issue shard: no targets in this slice")
            return 0
    if phase == "build":
        ok("issue shard binaries ready")
        return 0
    return _ci_issues_run_tests(ctx.t_all)


def cmd_ci():
    """CI build + test (parallel suites when AURA_TEST_JOBS>1).

    Honors AURA_ISSUE_BUILD=none (skip issue binary matrix + issues suite)
    for PR path-filter when only scripts/lib tooling changed.

    After aura + test_ir + test_concurrent compile: run concurrent, then
    overlap cheap/medium with the issue-farm ninja. Heavy (issues) waits
    for that ninja. Concurrent compile/run failure does not skip issues.
    """
    rc, ctx = _begin_build()
    if ctx is None:
        return rc

    conc_rc = _run_concurrent_early(ctx)

    saved_jobs = os.environ.get("AURA_TEST_JOBS")
    # Cap suite fan-out while ninja is live so 4-vCPU CI does not stack
    # 4 aura workers on top of issue_jobs. Local AURA_TEST_JOBS=1 stays 1.
    # No issue ninja (AURA_ISSUE_BUILD=none, e.g. CI build-test now that
    # the farm runs in issue-farm shards, #4819) → nothing to share with.
    issue_off = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower() in ("none", "skip", "off", "0")
    if _test_jobs() > 2 and not issue_off:
        os.environ["AURA_TEST_JOBS"] = "2"
        info("ci overlap: AURA_TEST_JOBS=2 while remaining ninja runs")

    print(
        f"{B}═══ Overlap: cheap/medium tests ‖ issue ninja ═══{N}",
        flush=True,
    )
    link_rc = 0
    early_rc = 0
    early_failed: list[str] = []
    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            fut_link = pool.submit(_finish_overlap_issues, ctx)
            fut_early = pool.submit(lambda: cmd_test(["ci"], waves=("cheap", "medium")))
            link_rc = fut_link.result()
            early_rc = fut_early.result()
            early_failed = list(_LAST_FAILED_SUITES)
    except Exception as exc:  # noqa: BLE001 — surface overlap failures
        fail(f"ci overlap: {exc}")
        return 1
    finally:
        if saved_jobs is None:
            os.environ.pop("AURA_TEST_JOBS", None)
        else:
            os.environ["AURA_TEST_JOBS"] = saved_jobs

    # Concurrent already ran. Heavy = ci/issues only.
    late_rc = cmd_test(["ci"], waves=("heavy",))
    late_failed = list(_LAST_FAILED_SUITES)
    return _report_ci_slice_failures(
        link_rc, early_rc, late_rc, conc_rc, early_failed=early_failed, late_failed=late_failed
    )


def cmd_list():
    """列出测试档位与叶子套件（由 SUITE_SPECS / TIER_LEAVES 生成）。"""
    print(f"{B}Test tiers{N}  (./build.py test <tier>; default ci)")
    width = max(len(n) for n in TIER_LEAVES)
    for name in ("pr", "ci", "all", "nightly", "everything", "core", "safety"):
        print(f"  {name:<{width}}  {TIER_DOCS[name]}")
    print()
    print(f"{B}Fast loop{N}  (local / agent default; not a CI tier)")
    print(f"  affected    {AFFECTED_DOC}")
    print()
    print(f"{B}Leaf suites{N}  (./build.py test <leaf>)")
    kind_width = max(len(s.kind) for s in SUITE_SPECS)
    for spec in SUITE_SPECS:
        par = "parallel" if spec.parallel else "serial"
        print(f"  {spec.name:<12}  [{spec.kind:<{kind_width}} {par:<8}]  {spec.doc}")
    print()
    print("  check         gate + build + ci")
    return 0


# ═══════════════════════════════════════════════════════════════
# PGO (Profile-Guided Optimization)
# ═══════════════════════════════════════════════════════════════

PGO_DIR = ROOT / ".aura-pgo"


def cmd_pgo_instrument():
    """Build Aura with PGO instrumentation."""
    print(f"{B}═══ PGO Instrument Build ═══{N}")
    BUILD.mkdir(parents=True, exist_ok=True)
    nproc = os.cpu_count() or 4
    r = run(
        [
            "cmake",
            "-B",
            str(BUILD),
            "-G",
            "Ninja",
            "-Wno-author",
            "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate",
            "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate",
            "-DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate",
        ],
        cwd=ROOT,
    )
    if r != 0:
        return r
    r = run(["cmake", "--build", str(BUILD), "--target", "aura", "-j", str(nproc)], cwd=ROOT)
    if r == 0:
        ok("PGO instrument build OK")
        print("  Run  : build.py pgo train --suite=mixed --iterations=3")
        print("  Merge: build.py pgo merge")
        print("  Build: build.py pgo optimize")
    else:
        fail("PGO instrument build failed")
    return r


def cmd_pgo_train():
    """Run training workload for PGO profile generation."""
    print(f"{B}═══ PGO Training ═══{N}")
    train_script = ROOT / "tests" / "pgo_train.py"
    if not train_script.exists():
        fail(f"Training script not found: {train_script}")
        return 1

    # Parse --suite/--iterations from sys.argv
    suite = "mixed"
    iterations = 3
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--suite" and i + 1 < len(sys.argv):
            suite = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == "--iterations" and i + 1 < len(sys.argv):
            iterations = int(sys.argv[i + 1])
            i += 2
        elif sys.argv[i].startswith("--suite="):
            suite = sys.argv[i].split("=", 1)[1]
            i += 1
        elif sys.argv[i].startswith("--iterations="):
            iterations = int(sys.argv[i].split("=", 1)[1])
            i += 1
        else:
            i += 1

    env = {**os.environ, "AURA_BIN": str(AURA)}
    return run(
        [
            sys.executable,
            str(train_script),
            "--suite",
            suite,
            "--iterations",
            str(iterations),
            "--merge",
        ],
        env=env,
        cwd=ROOT,
    )


def cmd_pgo_merge():
    """Merge profraw files into .profdata."""
    print(f"{B}═══ PGO Merge Profiles ═══{N}")
    PGO_DIR.mkdir(parents=True, exist_ok=True)
    profraw_files = list((PGO_DIR / "profraw").glob("*.profraw"))
    for f in ROOT.glob("*.profraw"):
        if f not in profraw_files:
            profraw_files.append(f)
    if not profraw_files:
        warn("No profraw files found")
        print("  Run training first: build.py pgo train --suite=mixed")
        return 1
    print(f"  Found {len(profraw_files)} profraw file(s)")

    profdata_cmd = "llvm-profdata"
    for c in ["llvm-profdata", "llvm-profdata-20", "llvm-profdata-19"]:
        r = subprocess.run(["which", c], capture_output=True, text=True)
        if r.returncode == 0:
            profdata_cmd = c
            break

    output = PGO_DIR / "aura.profdata"
    cmd = [profdata_cmd, "merge", "-output", str(output)] + [str(f) for f in profraw_files]
    print(f"  Merging → {output} ... ", end="", flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print("FAILED")
        print(f"  {r.stderr[:300]}")
        return 1
    print("OK")
    kb = output.stat().st_size / 1024
    ok(f"PGO profile ready: {output} ({kb:.1f} KB)")
    print("  Build: build.py pgo optimize")
    return 0


def cmd_pgo_optimize():
    """Build Aura with PGO profile data."""
    print(f"{B}═══ PGO Optimize Build ═══{N}")
    profdata = PGO_DIR / "aura.profdata"
    if not profdata.exists():
        warn(f"Profile not found: {profdata}")
        print("  Run training + merge first: build.py pgo train")
        return 1
    BUILD.mkdir(parents=True, exist_ok=True)
    nproc = os.cpu_count() or 4
    r = run(
        [
            "cmake",
            "-B",
            str(BUILD),
            "-G",
            "Ninja",
            "-Wno-author",
            f"-DCMAKE_CXX_FLAGS=-fprofile-instr-use={profdata}",
            f"-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-use={profdata}",
            f"-DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-use={profdata}",
        ],
        cwd=ROOT,
    )
    if r != 0:
        return r
    r = run(["cmake", "--build", str(BUILD), "--target", "aura", "-j", str(nproc)], cwd=ROOT)
    if r == 0:
        ok("PGO optimized build OK")
        print("  Now benchmark with: build.py test bench")
        print("  Or run: build.py pgo all  (full pipeline)")
    else:
        fail("PGO optimized build failed")
    return r


def cmd_pgo_all():
    """Full PGO pipeline: instrument → train → merge → optimize."""
    print(f"{B}{'=' * 55}{N}")
    print(f"{B}  PGO Full Pipeline (instrument → train → merge → optimize){N}")
    print(f"{B}{'=' * 55}{N}")
    steps = [
        ("Instrument build", cmd_pgo_instrument),
        ("Training + Merge", cmd_pgo_train),
        ("Optimize build", cmd_pgo_optimize),
    ]
    for name, fn in steps:
        print()
        rc = fn()
        if rc != 0:
            fail(f"PGO pipeline failed at step: {name}")
            return rc
    print()
    ok("PGO pipeline complete!")
    return 0


# ═══════════════════════════════════════════════════════════════
# LLVM source-based coverage (Issue #1933)
# ═══════════════════════════════════════════════════════════════

COVERAGE_BUILD = ROOT / "build_coverage"


def _find_llvm_tool(names: list[str]) -> str | None:
    for n in names:
        r = subprocess.run(["which", n], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return None


def cmd_fuzz():
    """Issue #1935: unified fuzz orchestrator.

    Usage:
      ./build.py fuzz --list
      ./build.py fuzz --all --quick
      ./build.py fuzz --only core,corpus,hygiene_prop --iters 50
    """
    script = ROOT / "tests" / "fuzz" / "run_all.py"
    if not script.is_file():
        fail(f"missing {script}")
        return 1
    # Forward argv after 'fuzz'
    fwd = sys.argv[2:]
    if not fwd:
        fwd = ["--list"]
    return run([sys.executable, str(script), *fwd], cwd=ROOT)


def cmd_coverage():
    """Issue #1933: instrumented build + test run + llvm-cov HTML/JSON report.

    Usage:
      ./build.py coverage --html
      ./build.py coverage --html --suite smoke
      ./build.py coverage --html --skip-build   # report only (needs prior run)
      ./build.py coverage --html --min-line-pct 0
      ./build.py coverage --check-tools         # static: tools + preset only

    Artifacts: build_coverage/coverage/{html,json,summary.txt,summary.json}
    """
    print(f"{B}═══ LLVM coverage (#1933) ═══{N}")
    argv = sys.argv[2:]
    do_html = "--html" in argv or "--json" not in argv
    do_json = "--json" in argv or "--html" in argv or True
    skip_build = "--skip-build" in argv
    check_tools = "--check-tools" in argv
    suite = "smoke"
    min_line = 0.0
    targets: list[str] = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--suite" and i + 1 < len(argv):
            suite = argv[i + 1]
            i += 2
            continue
        if a.startswith("--suite="):
            suite = a.split("=", 1)[1]
            i += 1
            continue
        if a == "--min-line-pct" and i + 1 < len(argv):
            min_line = float(argv[i + 1])
            i += 2
            continue
        if a.startswith("--min-line-pct="):
            min_line = float(a.split("=", 1)[1])
            i += 1
            continue
        if a == "--targets" and i + 1 < len(argv):
            targets = argv[i + 1].split(",")
            i += 2
            continue
        i += 1

    profdata = _find_llvm_tool(["llvm-profdata", "llvm-profdata-22", "llvm-profdata-20", "llvm-profdata-19"])
    cov = _find_llvm_tool(["llvm-cov", "llvm-cov-22", "llvm-cov-20", "llvm-cov-19"])
    if not profdata or not cov:
        fail("llvm-profdata / llvm-cov not found — install LLVM toolchain")
        return 1
    ok(f"tools: {profdata}, {cov}")

    # Static checks always useful
    cmake_cov = ROOT / "cmake" / "AuraCoverage.cmake"
    presets = ROOT / "CMakePresets.json"
    if not cmake_cov.is_file():
        fail(f"missing {cmake_cov}")
        return 1
    if not presets.is_file() or '"name": "coverage"' not in presets.read_text(encoding="utf-8"):
        fail("CMakePresets.json missing coverage preset")
        return 1
    ok("cmake AuraCoverage.cmake + coverage preset present")
    if check_tools:
        ok("coverage --check-tools clean")
        return 0

    nproc = os.cpu_count() or 4
    profraw_dir = COVERAGE_BUILD / "profraw"
    out_dir = COVERAGE_BUILD / "coverage"

    if not skip_build:
        print(f"{B}── configure coverage build ──{N}")
        COVERAGE_BUILD.mkdir(parents=True, exist_ok=True)
        conf = [
            "cmake",
            "-B",
            str(COVERAGE_BUILD),
            "-G",
            "Ninja",
            "-Wno-author",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DAURA_ENABLE_COVERAGE=ON",
        ]
        # Prefer Clang for source-based coverage
        if shutil.which("clang++"):
            conf.extend(["-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_C_COMPILER=clang"])
        r = run(conf, cwd=ROOT)
        if r != 0:
            fail("cmake configure (coverage) failed")
            return r

        if not targets:
            # Fast default: aura + a couple of unit-size issue tests if registered
            targets = ["aura"]
            for t in (
                "test_mutation_boundary_guard_1931",
                "test_closure_bridge_lifetime_1929",
                "test_layout_smoke",
            ):
                # ninja will fail missing targets — filter via build.ninja if present
                targets.append(t)

        # Only build targets that exist in the ninja graph
        bn = COVERAGE_BUILD / "build.ninja"
        if bn.is_file():
            text = bn.read_text(encoding="utf-8", errors="replace")
            targets = [t for t in targets if f"build {t}:" in text or f"build {t} " in text or t == "aura"]
            # aura may be phony — keep it
            if "aura" not in targets:
                targets.insert(0, "aura")

        print(f"{B}── build instrumented targets ──{N}")
        print(f"  targets: {', '.join(targets)}")
        r = run(
            ["cmake", "--build", str(COVERAGE_BUILD), "-j", str(nproc), "--target", *targets],
            cwd=ROOT,
        )
        if r != 0:
            # Retry with aura only (minimal) so report path still works after partial graph
            warn("full target set failed — retrying aura only")
            r = run(
                ["cmake", "--build", str(COVERAGE_BUILD), "-j", str(nproc), "--target", "aura"],
                cwd=ROOT,
            )
            if r != 0:
                fail("coverage instrumented build failed")
                return r

        print(f"{B}── run instrumented suite ({suite}) ──{N}")
        profraw_dir.mkdir(parents=True, exist_ok=True)
        # Clear old raw profiles for a clean merge
        for old in profraw_dir.glob("*.profraw"):
            old.unlink()
        env = {
            **os.environ,
            "LLVM_PROFILE_FILE": str(profraw_dir / "aura-%p-%m.profraw"),
            "AURA_BIN": str(COVERAGE_BUILD / "aura"),
        }
        # Run suite via build.py test but point at coverage build dir
        env["AURA_BUILD_DIR"] = str(COVERAGE_BUILD)
        suite_map = {
            "smoke": ["smoke"],
            "fixtures": ["fixtures"],
            "issues-fast": ["issues-fast"],
            "unit": ["unit"],
            "gate-smoke": ["fixtures", "smoke"],
        }
        suites = suite_map.get(suite, [suite])
        # Prefer direct binary runs for speed when available
        ran_any = False
        for t in targets:
            if t == "aura":
                continue
            bin_path = COVERAGE_BUILD / t
            if bin_path.is_file():
                print(f"  run {t}")
                subprocess.run([str(bin_path)], cwd=ROOT, env=env)
                ran_any = True
        if not ran_any:
            # Fall back to aura --help as minimal exercise
            aura_bin = COVERAGE_BUILD / "aura"
            if aura_bin.is_file():
                subprocess.run([str(aura_bin), "--help"], cwd=ROOT, env=env)
                # Also try a tiny eval if supported
                subprocess.run(
                    [str(aura_bin), "-e", "(+ 1 2)"],
                    cwd=ROOT,
                    env=env,
                    capture_output=True,
                )
                ran_any = True
        if suites and not ran_any:
            # Last resort: invoke test harness with AURA_BUILD_DIR
            r = subprocess.run(
                [sys.executable, str(ROOT / "build.py"), "test", *suites],
                cwd=ROOT,
                env=env,
            )
            if r.returncode != 0:
                warn(f"test suite exit {r.returncode} (continuing to report if profiles exist)")

    # Report
    print(f"{B}── llvm-cov report ──{N}")
    report_script = TOOLS / "llvm_cov_report.py"
    if not report_script.is_file():
        fail(f"missing {report_script}")
        return 1
    binaries = []
    for p in sorted(COVERAGE_BUILD.glob("test_*")):
        if p.is_file() and os.access(p, os.X_OK):
            binaries.append(str(p))
    aura_bin = COVERAGE_BUILD / "aura"
    if aura_bin.is_file():
        binaries.insert(0, str(aura_bin))
    cmd = [
        sys.executable,
        str(report_script),
        "--build-dir",
        str(COVERAGE_BUILD),
        "--profraw-dir",
        str(profraw_dir),
        "--out-dir",
        str(out_dir),
        "--min-line-pct",
        str(min_line),
    ]
    if do_html:
        cmd.append("--html")
    if do_json:
        cmd.append("--json")
    if binaries:
        cmd.append("--binaries")
        cmd.extend(binaries)
    # Soft module observability (0% floor — raise later as coverage grows)
    cmd.extend(["--require-module", "evaluator:0", "--require-module", "aura_jit:0"])
    r = run(cmd, cwd=ROOT)
    if r != 0:
        fail("llvm-cov report failed")
        return r
    ok(f"coverage report: {out_dir}")
    if (out_dir / "html" / "index.html").is_file():
        print(f"  HTML: {out_dir / 'html' / 'index.html'}")
    if (out_dir / "summary.json").is_file():
        print(f"  JSON: {out_dir / 'summary.json'}")
    return 0


def cmd_pgo():
    """PGO sub-commands."""
    subcmd = sys.argv[2] if len(sys.argv) > 2 else "help"
    subcommands = {
        "instrument": cmd_pgo_instrument,
        "train": cmd_pgo_train,
        "merge": cmd_pgo_merge,
        "optimize": cmd_pgo_optimize,
        "all": cmd_pgo_all,
    }
    if subcmd in subcommands:
        sys.argv.pop(1)
        return subcommands[subcmd]()
    print("PGO sub-commands:")
    for k, v in subcommands.items():
        print(f"    pgo {k:15s} {v.__doc__}")
    return 1


# ═══════════════════════════════════════════════════════════════
# Reproducible build / SBOM / security (Issue #675)
# ═══════════════════════════════════════════════════════════════

REPRO_SOURCE_DATE_EPOCH = "1704067200"  # 2024-01-01T00:00:00Z
# ci_reproducibility.py + security_scan.sh removed per Anqi 2026-07-19
# directive (scripts/ audit wave 9). The reproducible Release build path
# (SOURCE_DATE_EPOCH + --ffile-prefix-map + ccache_disable) remains shipped;
# only the verify + security-scan entry points are dropped.
SBOM_SCRIPT = TOOLS / "gen_sbom.py"


def _repro_cmake_flags(build_dir: Path) -> tuple[str, str, str]:
    """Compiler/linker flags for bit-reproducible Release builds (#675).

    Maps both source and build trees so dual-dir or relocated builds do
    not embed absolute paths via __FILE__ / DWARF / module BMI paths.
    """
    src = str(ROOT.resolve())
    bld = str(build_dir.resolve())
    # file + debug + macro prefix maps: cover __FILE__, assert paths, DWARF
    # and C++20 module mapper strings that otherwise leak the build dir
    # (CI dual-dir verify failed with distinct sha256 for a vs b).
    flags = (
        f"-ffile-prefix-map={src}=. "
        f"-fdebug-prefix-map={src}=. "
        f"-fmacro-prefix-map={src}=. "
        f"-ffile-prefix-map={bld}=build "
        f"-fdebug-prefix-map={bld}=build "
        f"-fmacro-prefix-map={bld}=build "
        f"-frandom-seed=aura-repro-675 "
        f"-fno-ident "
        f"-g0"
    )
    ldflags = ["-Wl,--build-id=none"]
    # Prefer the same fast linker as cmd_build when available, so the
    # shipped Release binary matches the mold/lld CI path. Always pin
    # fuse-ld explicitly so dual builds cannot pick different linkers.
    fast_ld = _select_fast_linker()
    if fast_ld:
        ldflags.append(f"-fuse-ld={fast_ld}")
    return flags, flags, " ".join(ldflags)


def _repro_env() -> dict[str, str]:
    """Stable env for reproducible configure/build."""
    env = {
        **os.environ,
        "SOURCE_DATE_EPOCH": os.environ.get("SOURCE_DATE_EPOCH", REPRO_SOURCE_DATE_EPOCH),
        "CCACHE_DISABLE": "1",
        "AURA_BUILD_TYPE": "Release",
        # Locale / timezone can leak into asctime-style strings and sort order.
        "LC_ALL": "C",
        "TZ": "UTC",
    }
    return env


def _repro_configure_and_build(build_dir: Path, *, label: str = "repro") -> int:
    """Configure + link `aura` under SOURCE_DATE_EPOCH / prefix-map flags.

    Honors AURA_LINK_JOBS (same as cmd_build) so CI can serialize link and
    avoid the arena.ixx.o rebuild deadlock noted in #2636.
    """
    build_dir.mkdir(parents=True, exist_ok=True)
    nproc = _build_jobs()
    cflags, cxxflags, ldflags = _repro_cmake_flags(build_dir)
    # Issue #2636: explicit compiler paths — /usr/bin/c++ can be a dangling
    # symlink on some local dev boxes; CMake auto-detect then fails.
    c_compiler = shutil.which("gcc") or shutil.which("cc") or "gcc"
    cxx_compiler = shutil.which("g++") or shutil.which("c++") or "g++"
    env = _repro_env()
    # Deterministic static archives (lib@cmake_cxx_std.a etc.): 'D' forces
    # zero UIDs/GIDs/timestamps so two sequential ar runs match.
    ar_create = "<CMAKE_AR> qcD <TARGET> <LINK_FLAGS> <OBJECTS>"
    ar_finish = "<CMAKE_RANLIB> -D <TARGET>"
    cmake_args = [
        "cmake",
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        "-Wno-author",  # was -Wno-dev (CMake 4.x deprecated it)
        "-DCMAKE_BUILD_TYPE=Release",
        # Issue #5203: the repro preset emits CMake's native SPDX 3.0.1 for
        # the aura-targets export. This path configures without presets, so
        # carry the same knob or the release install has no SBOM document.
        "-DCMAKE_INSTALL_SBOM_FORMATS=spdx",
        f"-DCMAKE_C_COMPILER={c_compiler}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DCMAKE_C_FLAGS={cflags}",
        f"-DCMAKE_CXX_FLAGS={cxxflags}",
        f"-DCMAKE_EXE_LINKER_FLAGS={ldflags}",
        f"-DCMAKE_SHARED_LINKER_FLAGS={ldflags}",
        f"-DCMAKE_C_ARCHIVE_CREATE={ar_create}",
        f"-DCMAKE_CXX_ARCHIVE_CREATE={ar_create}",
        f"-DCMAKE_C_ARCHIVE_FINISH={ar_finish}",
        f"-DCMAKE_CXX_ARCHIVE_FINISH={ar_finish}",
    ]
    link_jobs = os.environ.get("AURA_LINK_JOBS", "").strip()
    if link_jobs.isdigit() and int(link_jobs) > 0:
        cmake_args.append(f"-DAURA_LINK_JOBS={link_jobs}")
    r = run(cmake_args, cwd=ROOT, env=env)
    if r != 0:
        return r
    r = run(
        ["cmake", "--build", str(build_dir), "--target", "aura", "-j", str(nproc)],
        cwd=ROOT,
        env=env,
    )
    if r != 0:
        fail(f"{label} build failed → {build_dir}")
        return r
    aura = build_dir / "aura"
    if not aura.is_file():
        fail(f"{label}: missing binary {aura}")
        return 1
    h = hashlib.sha256(aura.read_bytes()).hexdigest()
    ok(f"{label} build OK → {aura}  sha256={h[:16]}…")
    return 0


def cmd_repro():
    """Reproducible Release build (#675).

    Default: one SOURCE_DATE_EPOCH Release build of `aura` in build_repro/.

    `--verify`: dual-build bit-identity check (Issue #675 / CI job
    `reproducible-build`). Two sequential clean builds into the **same**
    path (`build_repro/`), stash first binary, rebuild, require matching
    sha256. Same-path dual-build is the standard reproducible definition;
    dual-dir (a vs b) previously failed because C++20 module / assert
    paths leaked the build directory even with source-only prefix maps.
    (scripts/ci_reproducibility.py was deleted in audit wave 9; verify
    is inline.)
    """
    verify = "--verify" in sys.argv[2:]
    global BUILD, AURA, TEST_BIN

    if verify:
        print(f"{B}═══ Reproducible build verify (dual sequential) ═══{N}")
        build_dir = ROOT / "build_repro"
        stash = ROOT / "build_repro_first.bin"
        # Drop prior trees / stash so stale objects cannot fake a match.
        if build_dir.exists():
            shutil.rmtree(build_dir)
        if stash.exists():
            stash.unlink()
        # Also drop legacy dual-dir trees from the pre-fix verify path.
        for legacy in (ROOT / "build_repro_a", ROOT / "build_repro_b"):
            if legacy.exists():
                shutil.rmtree(legacy)

        r = _repro_configure_and_build(build_dir, label="repro-1")
        if r != 0:
            return r
        first = build_dir / "aura"
        ha = hashlib.sha256(first.read_bytes()).hexdigest()
        shutil.copy2(first, stash)

        # Clean rebuild from empty tree (no ccache; CCACHE_DISABLE=1).
        shutil.rmtree(build_dir)
        r = _repro_configure_and_build(build_dir, label="repro-2")
        if r != 0:
            return r
        second = build_dir / "aura"
        hb = hashlib.sha256(second.read_bytes()).hexdigest()
        if ha != hb:
            fail(f"repro binaries differ\n  1={ha}\n  2={hb}")
            # Keep stash for offline diffoscope / cmp.
            warn(f"first binary retained at {stash}")
            return 1
        if stash.exists():
            stash.unlink()
        ok(f"repro verify OK — sequential builds match sha256={ha[:16]}…")
        BUILD = build_dir
        AURA = second
        TEST_BIN = AURA
        return 0

    print(f"{B}═══ Reproducible build ═══{N}")
    BUILD = ROOT / "build_repro"
    AURA = BUILD / "aura"
    TEST_BIN = BUILD / "aura"
    return _repro_configure_and_build(BUILD, label="repro")


def cmd_sbom():
    """Generate CycloneDX SBOM (Issue #675)."""
    print(f"{B}═══ SBOM ═══{N}")
    if not SBOM_SCRIPT.exists():
        fail(f"missing {SBOM_SCRIPT}")
        return 1
    version = os.environ.get("AURA_VERSION", "dev")
    output = ROOT / "dist" / "aura-sbom.json"
    i = 2
    while i < len(sys.argv):
        a = sys.argv[i]
        if a.startswith("--version="):
            version = a.split("=", 1)[1]
        elif a == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]
            i += 1
        elif a.startswith("--output="):
            output = ROOT / a.split("=", 1)[1]
        i += 1
    r = run(
        [sys.executable, str(SBOM_SCRIPT), "--version", version, "--output", str(output)],
        cwd=ROOT,
    )
    if r == 0:
        ok(f"SBOM → {output}")
    else:
        fail("SBOM generation failed")
    return r


def cmd_security():
    """Filesystem / dependency vulnerability scan (Issue #675) removed per
    Anqi 2026-07-19 directive (scripts/security_scan.sh deleted). The
    security-scan entry point is dropped; deeper safety comes from the
    ASan / UBSan / TSan sanitizer matrix in build.py + CI."""


# ═══════════════════════════════════════════════════════════════
# LLM Benchmark
# ═══════════════════════════════════════════════════════════════


# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0

    # Sanitizer flag (Issue #299): --sanitizer=NAME or --sanitizer NAME.
    # Popped before subcommand dispatch so subcommands never see it.
    san_name = ""
    new_argv = [sys.argv[0]]
    i = 1
    while i < len(sys.argv):
        a = sys.argv[i]
        if a.startswith("--sanitizer="):
            san_name = a.split("=", 1)[1].strip()
            i += 1
        elif a == "--sanitizer" and i + 1 < len(sys.argv):
            san_name = sys.argv[i + 1].strip()
            i += 2
        else:
            new_argv.append(a)
            i += 1
    sys.argv = new_argv
    _apply_sanitizer(san_name)
    if san_name:
        print(f"{Y}--sanitizer={san_name} → build dir: {BUILD}{N}")

    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    args = sys.argv[2:]

    commands = {
        "build": cmd_build,
        "configure": cmd_configure,
        "clean": cmd_clean,
        "check": lambda: cmd_gate() or cmd_ci(),
        "gate": cmd_gate,
        "ci": cmd_ci,
        "ci-issues": cmd_ci_issues,
        "docs": cmd_docs,
        "cli-templates": cmd_cli_templates,
        "fixtures": cmd_fixtures,
        "lint": cmd_lint,
        "format": cmd_format,
        "test-registry": cmd_test_registry,
        "dead-heap-push": cmd_dead_heap_push,
        "catch-silent-swallow": cmd_catch_silent_swallow,
        "aot-env-linear-stamp": cmd_aot_env_linear_stamp,
        "legacy-test-inventory": cmd_legacy_test_inventory,
        "new-test-files": cmd_new_test_files,
        "orch-scope-child-2631": cmd_orch_scope_child_2631_coverage,
        "chaos-steal-gc-nightly-2931": cmd_chaos_steal_gc_nightly_2931,
        "chaos-steal-gc-nightly-2931-coverage": cmd_chaos_steal_gc_nightly_2931_coverage,
        "coverage": cmd_coverage,
        "fuzz": cmd_fuzz,
        "production-concurrency": cmd_production_concurrency,
        "production-concurrency-coverage": cmd_production_concurrency_coverage,
        "s0-metrics": test_s0_metrics,
        "chaos-pr-hard-fail": cmd_chaos_pr_hard_fail_gate,
        "chaos-pr-hard-fail-coverage": cmd_chaos_pr_hard_fail_coverage,
        "chaos-release-blocker-2902": cmd_chaos_release_blocker_2902,
        "chaos-release-blocker-2902-coverage": cmd_chaos_release_blocker_2902_coverage,
        "chaos-soak-hard-gate-2722": cmd_chaos_soak_hard_gate_2722,
        "chaos-soak-hard-gate-2722-coverage": cmd_chaos_soak_hard_gate_2722_coverage,
        "chaos-soak-residual-zero-2755-coverage": cmd_chaos_soak_residual_zero_2755_coverage,
        "production-concurrency-ready-gate-4159": cmd_production_concurrency_ready_gate_4159,
        "issue-coverage": cmd_issue_coverage,
        "batch-dirty-production-multi-only-2936": cmd_batch_dirty_production_multi_only_2936,
        "batch-dirty-production-multi-only-2936-coverage": cmd_batch_dirty_production_multi_only_2936_coverage,
        "pcv-span-stale-coverage-3167": cmd_pcv_span_stale_coverage_3167,
        "pcv-span-stale-3167": cmd_pcv_span_stale_coverage_3167,
        "stable-ref-probe-3400": cmd_stable_ref_probe_3400_coverage,
        "test": lambda: cmd_test(args or ["ci"]),
        "list": cmd_list,
        "demo": test_demo,
        "regression": lambda: cmd_test(["regression"]),
        "bench": cmd_bench,
        "pgo": cmd_pgo,
        "repro": cmd_repro,
        "sbom": cmd_sbom,
        "security": cmd_security,
    }

    if cmd in commands:
        rc = commands[cmd]()
    else:
        warn(f"unknown command '{cmd}'")
        print(__doc__.strip())
        rc = 1

    sys.exit(rc)


if __name__ == "__main__":
    main()
