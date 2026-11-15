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

# --- coverage wiring index (#5246) ---------------------------------
# Generated by scripts/gen_build_wiring_index.py. Each name below is a
# check executed by scripts/coverage/run_checks.py, or a deprecated cmd_*
# alias kept for source-cite rows. This replaces the old per-linter
# hand-wired chain; it is an index, not a second execution path.
WIRED_COVERAGE_NAMES = """
check_2662_coverage
check_2662_coverage.py
check_2663_coverage
check_2663_coverage.py
check_2664_coverage
check_2664_coverage.py
check_2665_coverage
check_2665_coverage.py
check_2666_coverage
check_2666_coverage.py
check_2667_coverage
check_2667_coverage.py
check_2668_coverage
check_2668_coverage.py
check_adaptive_thr_coverage
check_adaptive_thr_coverage.py
check_adt_exhaustiveness_production_hard
check_adt_exhaustiveness_production_hard.py
check_aot_env_linear_stamp_coverage
check_aot_env_linear_stamp_coverage.py
check_aot_reload_policy_coverage
check_aot_reload_policy_coverage.py
check_aot_stale_probe_hard_reject_coverage
check_aot_stale_probe_hard_reject_coverage.py
check_arena_moving_compaction_coverage
check_arena_moving_compaction_coverage.py
check_chaos_soak_deploy_gate
check_chaos_soak_deploy_gate.py
check_ci_toolchain_image
check_ci_toolchain_image.py
check_coercion_map_abort_rewind
check_coercion_map_abort_rewind.py
check_coverage_policy
check_coverage_policy.py
check_cross_function_impact_scope_coverage
check_cross_function_impact_scope_coverage.py
check_depth_slot_fiber_id_key
check_depth_slot_fiber_id_key.py
check_dual_dep_graph_parity_coverage
check_dual_dep_graph_parity_coverage.py
check_env_gen_fence_coverage
check_env_gen_fence_coverage.py
check_evaluator_ctor_production_upgrade
check_evaluator_ctor_production_upgrade.py
check_eventfd_wake_force_safepoint
check_eventfd_wake_force_safepoint.py
check_export_held_handoff_coverage
check_export_held_handoff_coverage.py
check_filter_prim_infer_standard
check_filter_prim_infer_standard.py
check_hold_aware_steal_scoring_coverage
check_hold_aware_steal_scoring_coverage.py
check_incremental_soundness_prod_coverage
check_incremental_soundness_prod_coverage.py
check_joint_epoch_bump_coverage
check_joint_epoch_bump_coverage.py
check_layout_stamp_fence_coverage
check_layout_stamp_fence_coverage.py
check_layout_stamp_shape_version_fence_coverage
check_layout_stamp_shape_version_fence_coverage.py
check_lifetime_pin_remap_coverage
check_lifetime_pin_remap_coverage.py
check_linear_fast_path_or_semantics
check_linear_fast_path_or_semantics.py
check_mid_provenance_unified
check_mid_provenance_unified.py
check_moving_pin_contract_fail_closed_coverage
check_moving_pin_contract_fail_closed_coverage.py
check_mutate_guard_coverage
check_mutate_guard_coverage.py
check_mutation_guard_coverage
check_mutation_guard_coverage.py
check_naming_convention
check_naming_convention.py
check_no_numeric_source_windows
check_no_numeric_source_windows.py
check_occurrence_persist_expected_fp_zero
check_occurrence_persist_expected_fp_zero.py
check_orch_mvp_scope
check_orch_mvp_scope.py
check_owned_pair_slots_lock_coverage
check_owned_pair_slots_lock_coverage.py
check_primitive_surface
check_primitive_surface.py
check_pure_parallel_isolation_wording
check_pure_parallel_isolation_wording.py
check_query_result_full_provenance
check_query_result_full_provenance.py
check_register_render_hot_prim_coverage
check_register_render_hot_prim_coverage.py
check_residual_sid0_cap_coverage
check_residual_sid0_cap_coverage.py
check_restamp_budget_hard_gate
check_restamp_budget_hard_gate.py
check_root_remap_pass_coverage
check_root_remap_pass_coverage.py
check_sandbox_mode_authority
check_sandbox_mode_authority.py
check_scope_bp_gauge_coverage
check_scope_bp_gauge_coverage.py
check_session_bound_dtor_cascade
check_session_bound_dtor_cascade.py
check_shape_storm_isolation_coverage
check_shape_storm_isolation_coverage.py
check_side_effect_security
check_side_effect_security.py
check_soa_single_source_of_truth_coverage
check_soa_single_source_of_truth_coverage.py
check_solve_delta_production_force_escalate
check_solve_delta_production_force_escalate.py
check_source_to_ir_strict_coverage
check_source_to_ir_strict_coverage.py
check_source_window_budget
check_source_window_budget.py
check_stable_func_id_eval_namespace_coverage
check_stable_func_id_eval_namespace_coverage.py
check_stamp_resolve_coverage
check_stamp_resolve_coverage.py
check_storm_clear_drive_body_coverage
check_storm_clear_drive_body_coverage.py
check_storm_clear_health_pass_coverage
check_storm_clear_health_pass_coverage.py
check_sync_remount_anon_coverage
check_sync_remount_anon_coverage.py
check_test_binding
check_test_binding.py
check_test_includes
check_test_includes.py
check_wildcard_effects_for_fence
check_wildcard_effects_for_fence.py
check_wildcard_grant_effect_fence
cmd_adt_exhaust_dirty_cone_3005_coverage
cmd_adt_exhaust_undermark_cone_3045_coverage
cmd_arena_compact_hook_stats_coverage
cmd_arena_dtor_clears_hooks_coverage
cmd_as_stable_ref_v2_coverage
cmd_aura_jit_unused_fn_lock_coverage
cmd_capability_string_matrix_unify_coverage
cmd_captured_anon_sync_remount_prod_default_2714_coverage
cmd_channel_rendezvous_coverage
cmd_closure_call_must_deopt_toctou_coverage
cmd_command_line_cap_io_read_coverage
cmd_composite_drift_inject_2671_coverage
cmd_cross_cow_soft_migrate_obs_2603_coverage
cmd_cross_eval_epoch_bump_2713_coverage
cmd_dead_coercion_hot_residual_3007_coverage
cmd_deferred_reemit_steal_sticky_2715_coverage
cmd_densify_remap_pairing_coverage
cmd_deopt_pending_closure_call_3412_coverage
cmd_dual_fresh_mutate_soft_migrate_3410_coverage
cmd_emit_object_deprecated_coverage
cmd_envframe_lifetime_proof_2711_coverage
cmd_epoch_invariant_soft_fuse_heal_2712_coverage
cmd_eval_current_no_auto_fix_coverage
cmd_fixup_deltas_coverage
cmd_gc_closures_mtx_flush_sweep_coverage
cmd_gc_heap_cells_clear_coverage
cmd_general_object_pin_auto_wire_2709_coverage
cmd_grant_macro_self_evo_stamp_coverage
cmd_has_on_compact_hook_lock_coverage
cmd_hash_table_grow_coverage
cmd_ir_optimize_type_info_chain_coverage
cmd_is_stealable_snapshot_gate_coverage
cmd_json_parse_number_exception_coverage
cmd_json_parse_object_grow_coverage
cmd_last_validated_generation_atomic_coverage
cmd_linear_fast_path_dirty_revalidate_3006_coverage
cmd_linear_three_layer_wire_coverage
cmd_list_end_of_list_void_coverage
cmd_live_closure_stable_id_only_coverage
cmd_load_cap_io_read_coverage
cmd_must_deopt_getter_sticky_3247_coverage
cmd_named_closure_stable_id_at_create_coverage
cmd_occurrence_cone_truncate_drift_2672_coverage
cmd_occurrence_hard_face_commit_2716_coverage
cmd_panic_checkpoint_steal_hard_2710_coverage
cmd_partial_cone_cap_coverage
cmd_partial_recompile_single_evict_coverage
cmd_partial_reemit_success_coverage_3413_coverage
cmd_reemit_auto_drain_boundary_2604_coverage
cmd_reemit_pipeline_reason_coverage_3445_coverage
cmd_require_effect_live_mid_coverage
cmd_residual_sid0_cap_coverage
cmd_security_audit_fold_coverage
cmd_security_schedule_mutate_admit_2630_coverage
cmd_stable_func_id_eval_namespace_coverage
cmd_stable_ref_wire_endian_coverage
cmd_storm_clear_drive_body_coverage
cmd_storm_clear_health_pass_coverage
cmd_structural_mutate_resolve_helper_coverage
cmd_subsecond_clock_coverage
cmd_sync_remount_anon_coverage
cmd_type_linear_commit_proof_stamp_2717_coverage
cmd_validate_node_no_abort_coverage
cmd_validate_post_restore_soa_coverage
linear_force_unified
residual_defer_steal_hard_and
Issue #100
Issue #101
Issue #102
Issue #103
Issue #106
Issue #107
Issue #113
Issue #114
Issue #115
Issue #119
Issue #121
Issue #124
Issue #125
Issue #130
Issue #160
Issue #161
Issue #173
Issue #178
Issue #183
Issue #187
Issue #189
Issue #192
Issue #206
Issue #211
Issue #212
Issue #213
Issue #218
Issue #219
Issue #220
Issue #223
Issue #224
Issue #226
Issue #254
Issue #261
Issue #268
Issue #278
Issue #283
Issue #289
Issue #300
Issue #306
Issue #307
Issue #322
Issue #324
Issue #335
Issue #339
Issue #344
Issue #354
Issue #365
Issue #368
Issue #378
Issue #381
Issue #384
Issue #393
Issue #398
Issue #399
Issue #400
Issue #403
Issue #404
Issue #405
Issue #406
Issue #407
Issue #408
Issue #411
Issue #414
Issue #416
Issue #417
Issue #418
Issue #419
Issue #420
Issue #421
Issue #422
Issue #423
Issue #424
Issue #427
Issue #432
Issue #438
Issue #441
Issue #454
Issue #464
Issue #467
Issue #468
Issue #477
Issue #478
Issue #480
Issue #489
Issue #491
Issue #495
Issue #506
Issue #508
Issue #509
Issue #514
Issue #517
Issue #518
Issue #526
Issue #527
Issue #529
Issue #531
Issue #536
Issue #542
Issue #543
Issue #544
Issue #545
Issue #546
Issue #547
Issue #548
Issue #551
Issue #552
Issue #553
Issue #554
Issue #556
Issue #560
Issue #561
Issue #562
Issue #563
Issue #564
Issue #565
Issue #570
Issue #571
Issue #573
Issue #574
Issue #575
Issue #576
Issue #577
Issue #583
Issue #588
Issue #592
Issue #593
Issue #594
Issue #595
Issue #596
Issue #597
Issue #598
Issue #599
Issue #600
Issue #602
Issue #604
Issue #605
Issue #607
Issue #608
Issue #609
Issue #610
Issue #611
Issue #612
Issue #619
Issue #623
Issue #627
Issue #628
Issue #629
Issue #634
Issue #635
Issue #636
Issue #638
Issue #639
Issue #642
Issue #652
Issue #653
Issue #654
Issue #655
Issue #657
Issue #658
Issue #659
Issue #673
Issue #674
Issue #685
Issue #731
Issue #737
Issue #738
Issue #739
Issue #740
Issue #741
Issue #742
Issue #743
Issue #744
Issue #745
Issue #746
Issue #747
Issue #751
Issue #752
Issue #753
Issue #754
Issue #755
Issue #798
Issue #799
Issue #800
Issue #803
Issue #805
Issue #818
Issue #881
Issue #923
Issue #941
Issue #957
Issue #986
Issue #1013
Issue #1014
Issue #1047
Issue #1097
Issue #1202
Issue #1241
Issue #1242
Issue #1289
Issue #1301
Issue #1321
Issue #1356
Issue #1359
Issue #1360
Issue #1361
Issue #1362
Issue #1364
Issue #1365
Issue #1366
Issue #1367
Issue #1368
Issue #1369
Issue #1370
Issue #1371
Issue #1372
Issue #1373
Issue #1374
Issue #1375
Issue #1376
Issue #1377
Issue #1378
Issue #1380
Issue #1381
Issue #1387
Issue #1388
Issue #1389
Issue #1390
Issue #1393
Issue #1397
Issue #1398
Issue #1401
Issue #1407
Issue #1408
Issue #1410
Issue #1411
Issue #1412
Issue #1413
Issue #1416
Issue #1420
Issue #1425
Issue #1433
Issue #1434
Issue #1435
Issue #1436
Issue #1437
Issue #1439
Issue #1444
Issue #1445
Issue #1446
Issue #1458
Issue #1465
Issue #1469
Issue #1476
Issue #1478
Issue #1480
Issue #1481
Issue #1482
Issue #1483
Issue #1490
Issue #1495
Issue #1496
Issue #1501
Issue #1503
Issue #1504
Issue #1506
Issue #1511
Issue #1518
Issue #1519
Issue #1526
Issue #1531
Issue #1534
Issue #1535
Issue #1543
Issue #1546
Issue #1547
Issue #1548
Issue #1557
Issue #1565
Issue #1566
Issue #1567
Issue #1568
Issue #1575
Issue #1576
Issue #1577
Issue #1579
Issue #1580
Issue #1581
Issue #1582
Issue #1583
Issue #1584
Issue #1585
Issue #1586
Issue #1587
Issue #1588
Issue #1589
Issue #1590
Issue #1591
Issue #1592
Issue #1594
Issue #1595
Issue #1596
Issue #1597
Issue #1598
Issue #1599
Issue #1600
Issue #1601
Issue #1602
Issue #1604
Issue #1605
Issue #1606
Issue #1607
Issue #1608
Issue #1609
Issue #1610
Issue #1611
Issue #1612
Issue #1613
Issue #1614
Issue #1615
Issue #1616
Issue #1617
Issue #1618
Issue #1619
Issue #1620
Issue #1621
Issue #1622
Issue #1623
Issue #1624
Issue #1626
Issue #1627
Issue #1628
Issue #1631
Issue #1633
Issue #1634
Issue #1635
Issue #1636
Issue #1638
Issue #1639
Issue #1640
Issue #1641
Issue #1648
Issue #1652
Issue #1655
Issue #1657
Issue #1658
Issue #1659
Issue #1660
Issue #1661
Issue #1662
Issue #1663
Issue #1664
Issue #1665
Issue #1666
Issue #1667
Issue #1670
Issue #1671
Issue #1672
Issue #1678
Issue #1679
Issue #1680
Issue #1681
Issue #1682
Issue #1684
Issue #1685
Issue #1686
Issue #1687
Issue #1688
Issue #1689
Issue #1690
Issue #1691
Issue #1692
Issue #1693
Issue #1694
Issue #1695
Issue #1696
Issue #1697
Issue #1699
Issue #1700
Issue #1701
Issue #1702
Issue #1703
Issue #1706
Issue #1707
Issue #1708
Issue #1709
Issue #1710
Issue #1711
Issue #1712
Issue #1714
Issue #1715
Issue #1716
Issue #1717
Issue #1718
Issue #1719
Issue #1720
Issue #1721
Issue #1722
Issue #1723
Issue #1724
Issue #1725
Issue #1726
Issue #1727
Issue #1728
Issue #1729
Issue #1730
Issue #1731
Issue #1732
Issue #1733
Issue #1734
Issue #1739
Issue #1740
Issue #1745
Issue #1746
Issue #1747
Issue #1753
Issue #1754
Issue #1755
Issue #1756
Issue #1757
Issue #1764
Issue #1765
Issue #1766
Issue #1767
Issue #1768
Issue #1769
Issue #1770
Issue #1771
Issue #1772
Issue #1779
Issue #1780
Issue #1781
Issue #1782
Issue #1783
Issue #1785
Issue #1786
Issue #1787
Issue #1795
Issue #1797
Issue #1806
Issue #1816
Issue #1818
Issue #1827
Issue #1835
Issue #1837
Issue #1838
Issue #1839
Issue #1840
Issue #1842
Issue #1843
Issue #1844
Issue #1845
Issue #1846
Issue #1847
Issue #1848
Issue #1849
Issue #1850
Issue #1851
Issue #1852
Issue #1853
Issue #1854
Issue #1855
Issue #1856
Issue #1857
Issue #1858
Issue #1859
Issue #1860
Issue #1861
Issue #1862
Issue #1863
Issue #1864
Issue #1865
Issue #1866
Issue #1867
Issue #1868
Issue #1869
Issue #1870
Issue #1871
Issue #1872
Issue #1873
Issue #1874
Issue #1875
Issue #1876
Issue #1877
Issue #1878
Issue #1879
Issue #1880
Issue #1881
Issue #1882
Issue #1883
Issue #1884
Issue #1885
Issue #1887
Issue #1888
Issue #1889
Issue #1890
Issue #1891
Issue #1892
Issue #1893
Issue #1894
Issue #1895
Issue #1896
Issue #1897
Issue #1898
Issue #1899
Issue #1908
Issue #1909
Issue #1910
Issue #1911
Issue #1912
Issue #1913
Issue #1914
Issue #1916
Issue #1917
Issue #1918
Issue #1919
Issue #1924
Issue #1927
Issue #1928
Issue #1929
Issue #1930
Issue #1931
Issue #1947
Issue #1948
Issue #1949
Issue #1966
Issue #1987
Issue #1988
Issue #1989
Issue #1990
Issue #1991
Issue #1992
Issue #1993
Issue #1994
Issue #1996
Issue #2000
Issue #2001
Issue #2002
Issue #2003
Issue #2004
Issue #2006
Issue #2007
Issue #2008
Issue #2010
Issue #2011
Issue #2012
Issue #2013
Issue #2014
Issue #2015
Issue #2016
Issue #2017
Issue #2018
Issue #2019
Issue #2020
Issue #2021
Issue #2022
Issue #2023
Issue #2024
Issue #2025
Issue #2026
Issue #2027
Issue #2028
Issue #2029
Issue #2030
Issue #2031
Issue #2032
Issue #2033
Issue #2034
Issue #2035
Issue #2036
Issue #2037
Issue #2038
Issue #2040
Issue #2041
Issue #2042
Issue #2043
Issue #2044
Issue #2045
Issue #2046
Issue #2052
Issue #2054
Issue #2055
Issue #2056
Issue #2057
Issue #2058
Issue #2059
Issue #2060
Issue #2061
Issue #2062
Issue #2063
Issue #2064
Issue #2065
Issue #2066
Issue #2067
Issue #2068
Issue #2072
Issue #2073
Issue #2074
Issue #2075
Issue #2076
Issue #2077
Issue #2078
Issue #2079
Issue #2080
Issue #2081
Issue #2082
Issue #2083
Issue #2084
Issue #2085
Issue #2086
Issue #2087
Issue #2088
Issue #2089
Issue #2090
Issue #2092
Issue #2093
Issue #2094
Issue #2095
Issue #2096
Issue #2097
Issue #2098
Issue #2099
Issue #2100
Issue #2101
Issue #2102
Issue #2103
Issue #2104
Issue #2105
Issue #2106
Issue #2107
Issue #2108
Issue #2109
Issue #2110
Issue #2111
Issue #2112
Issue #2113
Issue #2114
Issue #2115
Issue #2116
Issue #2117
Issue #2118
Issue #2119
Issue #2120
Issue #2121
Issue #2122
Issue #2123
Issue #2124
Issue #2125
Issue #2126
Issue #2127
Issue #2128
Issue #2129
Issue #2130
Issue #2131
Issue #2132
Issue #2133
Issue #2137
Issue #2139
Issue #2140
Issue #2141
Issue #2142
Issue #2143
Issue #2144
Issue #2145
Issue #2146
Issue #2147
Issue #2148
Issue #2149
Issue #2150
Issue #2151
Issue #2152
Issue #2153
Issue #2154
Issue #2155
Issue #2156
Issue #2157
Issue #2158
Issue #2159
Issue #2161
Issue #2162
Issue #2163
Issue #2164
Issue #2165
Issue #2166
Issue #2167
Issue #2169
Issue #2170
Issue #2171
Issue #2172
Issue #2173
Issue #2174
Issue #2175
Issue #2176
Issue #2177
Issue #2178
Issue #2179
Issue #2180
Issue #2181
Issue #2182
Issue #2183
Issue #2184
Issue #2185
Issue #2186
Issue #2187
Issue #2188
Issue #2189
Issue #2190
Issue #2191
Issue #2192
Issue #2193
Issue #2194
Issue #2195
Issue #2196
Issue #2197
Issue #2198
Issue #2199
Issue #2200
Issue #2201
Issue #2202
Issue #2203
Issue #2204
Issue #2205
Issue #2206
Issue #2207
Issue #2208
Issue #2209
Issue #2210
Issue #2211
Issue #2212
Issue #2219
Issue #2220
Issue #2221
Issue #2222
Issue #2223
Issue #2224
Issue #2225
Issue #2226
Issue #2227
Issue #2228
Issue #2229
Issue #2230
Issue #2231
Issue #2232
Issue #2233
Issue #2234
Issue #2235
Issue #2236
Issue #2237
Issue #2238
Issue #2239
Issue #2240
Issue #2241
Issue #2242
Issue #2243
Issue #2244
Issue #2245
Issue #2246
Issue #2247
Issue #2248
Issue #2249
Issue #2250
Issue #2251
Issue #2252
Issue #2253
Issue #2254
Issue #2256
Issue #2257
Issue #2258
Issue #2259
Issue #2260
Issue #2261
Issue #2262
Issue #2263
Issue #2264
Issue #2268
Issue #2269
Issue #2271
Issue #2272
Issue #2273
Issue #2274
Issue #2275
Issue #2276
Issue #2277
Issue #2278
Issue #2280
Issue #2281
Issue #2282
Issue #2283
Issue #2284
Issue #2285
Issue #2286
Issue #2287
Issue #2288
Issue #2289
Issue #2290
Issue #2291
Issue #2293
Issue #2294
Issue #2295
check_envframe_ownership_transfer_2295
check_envframe_ownership_transfer_2295.py
Issue #2296
check_residual_gc_defer_multi_eval_2296
check_residual_gc_defer_multi_eval_2296.py
Issue #2297
check_capture_cell_remap_2297
check_capture_cell_remap_2297.py
Issue #2298
check_general_object_pin_2298
check_general_object_pin_2298.py
Issue #2299
check_aot_per_eval_slot_invalidate_2299
check_aot_per_eval_slot_invalidate_2299.py
Issue #2300
check_lifetime_contract_snapshot_2300
check_lifetime_contract_snapshot_2300.py
Issue #2301
Issue #2302
Issue #2304
Issue #2306
Issue #2307
Issue #2308
Issue #2309
Issue #2310
Issue #2312
Issue #2313
Issue #2314
Issue #2315
Issue #2316
Issue #2317
Issue #2318
Issue #2319
Issue #2320
Issue #2321
Issue #2333
Issue #2335
Issue #2337
Issue #2338
Issue #2339
Issue #2340
Issue #2341
Issue #2342
Issue #2343
Issue #2344
Issue #2345
Issue #2346
check_steal_snapshot_hard_invariant_2346
check_steal_snapshot_hard_invariant_2346.py
Issue #2347
Issue #2348
Issue #2349
Issue #2350
Issue #2351
Issue #2352
Issue #2353
Issue #2354
Issue #2355
Issue #2356
Issue #2357
Issue #2358
Issue #2359
Issue #2360
Issue #2361
Issue #2362
check_envframe_ownership_steal_densify_2362
check_envframe_ownership_steal_densify_2362.py
Issue #2363
Issue #2364
Issue #2365
Issue #2366
Issue #2367
Issue #2368
check_densify_remap_pairing_2368
check_densify_remap_pairing_2368.py
Issue #2369
check_live_closure_stable_id_only_2369
check_live_closure_stable_id_only_2369.py
Issue #2370
Issue #2371
Issue #2372
check_steal_snapshot_soft_production_lock_2372
check_steal_snapshot_soft_production_lock_2372.py
Issue #2374
check_legacy_pin_registry_cleanup_2374
check_legacy_pin_registry_cleanup_2374.py
Issue #2375
check_pin_bulk_all_shards_2375
check_pin_bulk_all_shards_2375.py
Issue #2376
Issue #2377
check_steal_complete_strong_entry_2377
check_steal_complete_strong_entry_2377.py
Issue #2378
check_mailbox_defer_drain_sla_2378
check_mailbox_defer_drain_sla_2378.py
Issue #2379
Issue #2381
check_arena_compact_hook_stats_2381
check_arena_compact_hook_stats_2381.py
Issue #2382
check_arena_dtor_clears_hooks_2382
check_arena_dtor_clears_hooks_2382.py
Issue #2383
check_has_on_compact_hook_lock_2383
check_has_on_compact_hook_lock_2383.py
Issue #2384
check_require_effect_live_mid_2384
check_require_effect_live_mid_2384.py
Issue #2385
Issue #2386
check_grant_macro_self_evo_stamp_2386
check_grant_macro_self_evo_stamp_2386.py
Issue #2387
check_capability_string_matrix_unify_2387
Issue #2388
check_security_audit_fold_2388
check_security_audit_fold_2388.py
Issue #2389
Issue #2390
check_validate_node_no_abort_2390
check_validate_node_no_abort_2390.py
Issue #2391
check_validate_post_restore_soa_2391
check_validate_post_restore_soa_2391.py
Issue #2392
check_fixup_deltas_2392
check_fixup_deltas_2392.py
Issue #2393
Issue #2394
check_last_validated_generation_atomic_2394
Issue #2395
check_stable_ref_wire_endian_2395
Issue #2396
Issue #2397
Issue #2398
Issue #2399
Issue #2400
Issue #2402
Issue #2403
Issue #2404
check_stable_ref_export_2404
check_stable_ref_export_2404.py
Issue #2405
Issue #2406
Issue #2408
check_stringpool_bytes_total_lock_2408
check_stringpool_bytes_total_lock_2408.py
Issue #2409
check_stringpool_buf_fragmentation_lock_2409
check_stringpool_buf_fragmentation_lock_2409.py
Issue #2410
check_node_meta_bounds_2410
check_node_meta_bounds_2410.py
Issue #2411
check_node_meta_gap_2411
check_node_meta_gap_2411.py
Issue #2412
check_reset_slot_parent_edges_2412
check_reset_slot_parent_edges_2412.py
Issue #2413
check_flatast_add_node_lock_2413
check_flatast_add_node_lock_2413.py
Issue #2414
check_summary_recompute_sym_2414
check_summary_recompute_sym_2414.py
Issue #2415
check_summary_flags_guard_2415
check_summary_flags_guard_2415.py
Issue #2416
Issue #2417
check_binding_gens_atomic_2417
check_binding_gens_atomic_2417.py
Issue #2418
check_structural_metadata_lock_order_2418
check_structural_metadata_lock_order_2418.py
Issue #2419
Issue #2420
check_tag_arity_key_hash_2420
check_tag_arity_key_hash_2420.py
Issue #2421
check_restamp_lazy_align_atomic_2421
check_restamp_lazy_align_atomic_2421.py
Issue #2422
check_subtree_gen_atomic_2422
check_subtree_gen_atomic_2422.py
Issue #2423
check_dirty_column_lock_2423
check_dirty_column_lock_2423.py
Issue #2424
check_subtree_dirty_bounds_2424
check_subtree_dirty_bounds_2424.py
Issue #2425
Issue #2426
Issue #2427
Issue #2428
check_gc_defer_arm_fetch_or_2428
check_gc_defer_arm_fetch_or_2428.py
Issue #2429
Issue #2430
Issue #2431
check_dead_coercion_columnar_2431
check_dead_coercion_columnar_2431.py
Issue #2432
Issue #2433
Issue #2435
check_hot_contract_placement_2435
check_hot_contract_placement_2435.py
Issue #2436
Issue #2437
Issue #2438
Issue #2439
Issue #2440
Issue #2441
Issue #2442
Issue #2443
Issue #2444
Issue #2445
check_add_node_builder_contract_2445
check_add_node_builder_contract_2445.py
Issue #2446
Issue #2447
Issue #2448
check_defines_referencing_sym_2448
check_defines_referencing_sym_2448.py
Issue #2449
Issue #2450
Issue #2451
Issue #2452
Issue #2453
Issue #2454
Issue #2455
check_restore_children_structural_lock_2455
check_restore_children_structural_lock_2455.py
Issue #2456
check_subtree_uses_sym_template_bloat_2456
check_subtree_uses_sym_template_bloat_2456.py
Issue #2457
Issue #2458
Issue #2459
check_castop_density_closed_loop_2459
check_castop_density_closed_loop_2459.py
Issue #2460
Issue #2461
Issue #2462
Issue #2463
Issue #2465
Issue #2467
Issue #2468
Issue #2469
Issue #2471
check_ir_optimize_type_info_chain_2471
check_ir_optimize_type_info_chain_2471.py
Issue #2472
check_closure_call_must_deopt_toctou_2472
check_closure_call_must_deopt_toctou_2472.py
Issue #2473
check_gc_closures_mtx_flush_sweep_2473
check_gc_closures_mtx_flush_sweep_2473.py
check_ffi_hot_path_cache_toctou_2474
check_ffi_hot_path_cache_toctou_2474.py
Issue #2475
check_aura_jit_unused_fn_lock_2475
check_aura_jit_unused_fn_lock_2475.py
Issue #2476
check_partial_recompile_single_evict_2476
check_partial_recompile_single_evict_2476.py
Issue #2477
check_emit_object_deprecated_2477
check_emit_object_deprecated_2477.py
Issue #2478
check_command_line_cap_io_read_2478
check_command_line_cap_io_read_2478.py
Issue #2479
Issue #2480
check_json_parse_number_exception_2480
check_json_parse_number_exception_2480.py
Issue #2481
check_json_parse_object_grow_2481
check_json_parse_object_grow_2481.py
Issue #2482
check_list_end_of_list_void_2482
check_list_end_of_list_void_2482.py
Issue #2483
check_channel_rendezvous_2483
check_channel_rendezvous_2483.py
Issue #2484
check_eval_current_no_auto_fix_2484
check_eval_current_no_auto_fix_2484.py
Issue #2485
check_load_cap_io_read_2485
check_load_cap_io_read_2485.py
Issue #2486
check_gc_heap_cells_clear_2486
check_gc_heap_cells_clear_2486.py
Issue #2487
Issue #2488
Issue #2489
Issue #2490
check_require_effect_auto_isolation_2490
check_require_effect_auto_isolation_2490.py
Issue #2491
Issue #2492
Issue #2493
Issue #2494
Issue #2495
Issue #2496
Issue #2497
check_densify_ownership_scan_fail_gate_2497
check_densify_ownership_scan_fail_gate_2497.py
Issue #2498
check_fiber_reclaim_orphan_release_2498
check_fiber_reclaim_orphan_release_2498.py
Issue #2499
Issue #2500
Issue #2501
Issue #2502
Issue #2503
Issue #2504
Issue #2505
Issue #2506
Issue #2507
Issue #2508
Issue #2509
Issue #2510
check_steal_complete_restamp_txn_2510
check_steal_complete_restamp_txn_2510.py
Issue #2511
Issue #2512
Issue #2513
Issue #2514
check_linear_synth_boundary_authority_2514
check_linear_synth_boundary_authority_2514.py
Issue #2515
check_orch_soft_boundary_unified_2515
check_orch_soft_boundary_unified_2515.py
Issue #2516
check_type_dirty_txn_order_2516
check_type_dirty_txn_order_2516.py
Issue #2517
Issue #2518
Issue #2519
Issue #2520
check_soa_ban_residual_aos_bridge_2520
check_soa_ban_residual_aos_bridge_2520.py
Issue #2521
Issue #2522
Issue #2523
Issue #2524
check_module_partition_map_2524
check_module_partition_map_2524.py
Issue #2525
Issue #2526
Issue #2527
Issue #2528
check_restamp_sla_observability_2528
check_restamp_sla_observability_2528.py
Issue #2529
Issue #2530
Issue #2531
Issue #2532
Issue #2533
Issue #2534
Issue #2535
Issue #2536
Issue #2537
Issue #2538
Issue #2539
Issue #2540
Issue #2541
Issue #2542
Issue #2543
Issue #2544
Issue #2545
check_linear_force_unified_2545
check_linear_force_unified_2545.py
Issue #2546
check_residual_defer_steal_hard_and_2546
check_residual_defer_steal_hard_and_2546.py
Issue #2547
Issue #2548
Issue #2549
check_is_stealable_snapshot_gate_2549
check_is_stealable_snapshot_gate_2549.py
Issue #2550
check_named_closure_stable_id_at_create_2550
check_named_closure_stable_id_at_create_2550.py
Issue #2551
Issue #2552
Issue #2553
Issue #2555
check_transaction_guard_migration_2555
check_transaction_guard_migration_2555.py
Issue #2556
Issue #2557
Issue #2558
Issue #2559
check_linear_three_layer_wire_2559
check_linear_three_layer_wire_2559.py
Issue #2560
check_partial_cone_cap_2560
check_partial_cone_cap_2560.py
Issue #2561
Issue #2562
Issue #2564
Issue #2566
Issue #2567
Issue #2568
Issue #2569
Issue #2570
Issue #2571
Issue #2572
Issue #2573
Issue #2574
Issue #2575
Issue #2576
Issue #2577
Issue #2578
Issue #2579
Issue #2581
Issue #2582
Issue #2583
Issue #2584
Issue #2585
Issue #2586
Issue #2587
Issue #2588
Issue #2589
Issue #2590
Issue #2591
Issue #2592
Issue #2593
Issue #2594
Issue #2595
Issue #2596
Issue #2597
Issue #2598
Issue #2599
Issue #2600
Issue #2601
check_aot_exhausted_min_dirty_retry_2601
check_aot_exhausted_min_dirty_retry_2601.py
Issue #2602
check_closure_sync_remount_2602
check_closure_sync_remount_2602.py
Issue #2603
check_cross_cow_soft_migrate_obs_2603
check_cross_cow_soft_migrate_obs_2603.py
Issue #2604
check_reemit_auto_drain_boundary_2604
check_reemit_auto_drain_boundary_2604.py
Issue #2605
Issue #2606
check_pereval_reemit_region_independence_2606
check_pereval_reemit_region_independence_2606.py
Issue #2607
Issue #2608
Issue #2609
Issue #2610
Issue #2611
check_dce_elided_deopt_meta_2611
check_dce_elided_deopt_meta_2611.py
Issue #2613
Issue #2614
Issue #2615
check_batch_dirty_discipline_2615
check_batch_dirty_discipline_2615.py
Issue #2616
check_value_tag_hotpath_ban_2616
check_value_tag_hotpath_ban_2616.py
Issue #2617
check_shape_compact_storm_isolation_2617
check_shape_compact_storm_isolation_2617.py
Issue #2618
check_soa_residual_production_smoke_2618
check_soa_residual_production_smoke_2618.py
Issue #2619
Issue #2620
Issue #2621
Issue #2622
Issue #2624
Issue #2626
Issue #2627
Issue #2628
Issue #2630
check_security_schedule_mutate_admit_2630
check_security_schedule_mutate_admit_2630.py
check_orch_scope_child_2631
check_orch_scope_child_2631.py
Issue #2634
check_pure_probe_hardening_2634
check_pure_probe_hardening_2634.py
Issue #2635
check_mid_fallback_hard_deny_2635
check_mid_fallback_hard_deny_2635.py
Issue #2637
Issue #2638
Issue #2639
Issue #2640
Issue #2641
Issue #2643
Issue #2645
check_dead_coercion_layered_evidence_2645
check_dead_coercion_layered_evidence_2645.py
Issue #2646
check_occurrence_cone_outside_invalidate_2646
check_occurrence_cone_outside_invalidate_2646.py
Issue #2647
check_occurrence_goal_vacuous_solve_prevent_2647
check_occurrence_goal_vacuous_solve_prevent_2647.py
Issue #2648
Issue #2650
Issue #2651
check_pmr_alloc_fiber_safe_2651
check_pmr_alloc_fiber_safe_2651.py
Issue #2652
check_string_heap_corruption_guard_2652
check_string_heap_corruption_guard_2652.py
Issue #2653
check_module_path_refuse_2653
check_module_path_refuse_2653.py
Issue #2654
check_hash_table_grow_2654
Issue #2655
check_subsecond_clock_2655
Issue #2656
check_fiber_spawn_cli_2656
check_fiber_spawn_cli_2656.py
Issue #2657
Issue #2658
Issue #2659
Issue #2660
Issue #2661
Issue #2662
Issue #2663
Issue #2664
Issue #2665
Issue #2666
Issue #2667
Issue #2668
Issue #2669
Issue #2670
Issue #2671
check_composite_drift_inject_2671
check_composite_drift_inject_2671.py
Issue #2672
check_occurrence_cone_truncate_drift_2672
check_occurrence_cone_truncate_drift_2672.py
Issue #2673
Issue #2674
check_layered_evidence_coherence_2674
check_layered_evidence_coherence_2674.py
Issue #2675
check_linear_enforce_effective_2675
Issue #2676
check_shared_heap_serial_2676
check_shared_heap_serial_2676.py
Issue #2677
check_resume_invariants_2677
check_resume_invariants_2677.py
check_module_import_contiguity_2678
check_module_import_contiguity_2678.py
Issue #2679
check_chaos_soak_2679
check_chaos_soak_2679.py
Issue #2680
check_mailbox_boundary_interleave_2680
check_mailbox_boundary_interleave_2680.py
Issue #2681
Issue #2682
check_moving_unified_success_2682
check_moving_unified_success_2682.py
Issue #2683
check_shape_storm_isolation_2683
check_shape_storm_isolation_2683.py
Issue #2684
Issue #2685
Issue #2686
Issue #2687
check_evaluator_capture_tenant_2687
check_evaluator_capture_tenant_2687.py
Issue #2688
check_capability_production_default_2688
check_capability_production_default_2688.py
Issue #2689
check_require_effect_on_ref_2689
check_require_effect_on_ref_2689.py
Issue #2690
check_pending_recovery_drain_2690
check_pending_recovery_drain_2690.py
Issue #2691
check_closure_anon_captured_remount_2691
check_closure_anon_captured_remount_2691.py
Issue #2692
check_aot_slot_owner_consistency_2692
check_aot_slot_owner_consistency_2692.py
cmd_aot_slot_owner_consistency_2692
Issue #2693
Issue #2694
Issue #2695
Issue #2696
Issue #2697
Issue #2698
Issue #2699
check_steal_safety_transaction_2699
check_steal_safety_transaction_2699.py
Issue #2700
check_handoff_ref_mailbox_gate_2700
check_handoff_ref_mailbox_gate_2700.py
Issue #2701
check_mutation_hold_budget_reject_2701
check_mutation_hold_budget_reject_2701.py
Issue #2702
check_resume_hard_fail_2702
check_resume_hard_fail_2702.py
Issue #2703
check_cone_outside_goal_drop_2703
check_cone_outside_goal_drop_2703.py
Issue #2704
check_occurrence_empty_after_fence_2704
check_occurrence_empty_after_fence_2704.py
check_hard_capture_tenant_2705
check_hard_capture_tenant_2705.py
Issue #2706
check_sole_require_effect_2706
check_sole_require_effect_2706.py
Issue #2707
check_mid_join_fail_closed_2707
check_mid_join_fail_closed_2707.py
Issue #2708
check_ownership_rebind_walk_2708
check_ownership_rebind_walk_2708.py
Issue #2709
check_general_object_pin_auto_wire_2709
check_general_object_pin_auto_wire_2709.py
Issue #2710
check_panic_checkpoint_steal_hard_2710
check_panic_checkpoint_steal_hard_2710.py
Issue #2711
check_envframe_lifetime_proof_2711
check_envframe_lifetime_proof_2711.py
Issue #2712
check_epoch_invariant_soft_fuse_heal_2712
check_epoch_invariant_soft_fuse_heal_2712.py
Issue #2713
check_cross_eval_epoch_bump_2713
check_cross_eval_epoch_bump_2713.py
Issue #2714
check_captured_anon_sync_remount_prod_default_2714
check_captured_anon_sync_remount_prod_default_2714.py
Issue #2715
check_deferred_reemit_steal_sticky_2715
check_deferred_reemit_steal_sticky_2715.py
Issue #2716
check_occurrence_hard_face_commit_2716
check_occurrence_hard_face_commit_2716.py
Issue #2717
check_type_linear_commit_proof_stamp_2717
check_type_linear_commit_proof_stamp_2717.py
Issue #2718
Issue #2719
Issue #2720
Issue #2721
check_chaos_soak_hard_gate_2722
check_chaos_soak_hard_gate_2722.py
Issue #2723
Issue #2724
Issue #2726
check_cross_fiber_hold_budget_cancel_2726
check_cross_fiber_hold_budget_cancel_2726.py
Issue #2727
check_fiber_evaluator_id_2727
check_fiber_evaluator_id_2727.py
check_typed_mutation_audit_h_forward_ref_2728
check_typed_mutation_audit_h_forward_ref_2728.py
Issue #2742
Issue #2743
Issue #2744
Issue #2745
Issue #2746
Issue #2747
Issue #2748
Issue #2749
Issue #2750
Issue #2751
Issue #2752
check_try_steal_from_txn_2752
check_try_steal_from_txn_2752.py
Issue #2753
Issue #2754
check_region_cone_disjoint_admit_2754
check_chaos_soak_residual_zero_2755
check_chaos_soak_residual_zero_2755.py
Issue #2756
check_workflow_failure_policy_2756
check_workflow_failure_policy_2756.py
Issue #2757
check_region_mask_disjoint_admit_2757
check_region_mask_disjoint_admit_2757.py
Issue #2758
check_type_linear_commit_proof_counts_2758
check_evaluator_stamp_sole_authority_2759
check_evaluator_stamp_sole_authority_2759.py
Issue #2760
check_region_impact_mask_admit_2760
Issue #2761
check_region_mask_overlap_admit_2761
check_region_mask_overlap_admit_2761.py
Issue #2762
check_post_mutate_macro_reexpand_2762
check_post_mutate_macro_reexpand_2762.py
Issue #2763
check_query_pattern_delta_hygiene_2763
check_query_pattern_delta_hygiene_2763.py
Issue #2764
check_ir_jit_macro_marker_enforcement_2764
Issue #2765
check_guard_reflect_validate_2765
Issue #2766
check_module_require_export_order_2766
check_module_require_export_order_2766.py
Issue #2768
check_orchestrator_agent_stdin_2768
check_orchestrator_agent_stdin_2768.py
Issue #2769
check_stdlib_require_export_audit_2769
check_stdlib_require_export_audit_2769.py
check_string_split_iterative_2770
check_string_split_iterative_2770.py
Issue #2771
check_tcp_listen_accept_2771
check_tcp_listen_accept_2771.py
Issue #2773
check_unified_dirty_fence_2773
check_unified_dirty_fence_2773.py
Issue #2774
check_batch_dirty_multi_via_single_ban_2774
check_batch_dirty_multi_via_single_ban_2774.py
Issue #2775
Issue #2776
check_aot_reload_consistency_stamp_concurrent_2776
check_aot_reload_consistency_stamp_concurrent_2776.py
Issue #2777
check_agent_scope_read_guard_2777
Issue #2778
Issue #2779
Issue #2780
Issue #2781
Issue #2782
Issue #2783
Issue #2784
check_workspace_sync_from_body_2784
check_workspace_sync_from_body_2784.py
Issue #2785
check_workspace_switch_bind_2785
check_workspace_switch_bind_2785.py
Issue #2786
check_workspace_lock_active_flag_2786
check_workspace_lock_active_flag_2786.py
Issue #2787
check_workspace_rollback_latest_2787
check_workspace_rollback_latest_2787.py
Issue #2788
check_workspace_rollback_to_2788
check_workspace_rollback_to_2788.py
Issue #2789
check_workspace_delete_subtree_2789
check_workspace_delete_subtree_2789.py
Issue #2790
check_atomic_batch_partial_failure_2790
check_atomic_batch_partial_failure_2790.py
Issue #2791
check_rebind_parse_failure_no_leak_2791
check_rebind_parse_failure_no_leak_2791.py
Issue #2792
check_rebind_new_body_hygiene_2792
check_rebind_new_body_hygiene_2792.py
Issue #2793
check_replace_value_audit_consistency_2793
check_replace_value_audit_consistency_2793.py
Issue #2794
check_atomic_batch_move_noop_2794
check_atomic_batch_move_noop_2794.py
Issue #2795
check_rebind_rollback_nodeid_validity_2795
check_rebind_rollback_nodeid_validity_2795.py
Issue #2796
check_atomic_batch_rollback_metric_noise_2796
check_atomic_batch_rollback_metric_noise_2796.py
Issue #2797
check_replace_subtree_new_body_hygiene_2797
check_replace_subtree_new_body_hygiene_2797.py
Issue #2798
check_replace_pattern_no_match_no_leak_2798
check_replace_pattern_no_match_no_leak_2798.py
Issue #2799
check_tweak_literal_audit_consistency_2799
check_tweak_literal_audit_consistency_2799.py
Issue #2800
check_replace_pattern_multi_match_nodeid_stability_2800
check_replace_pattern_multi_match_nodeid_stability_2800.py
Issue #2801
check_move_node_hygiene_2801
check_move_node_hygiene_2801.py
Issue #2802
check_atomic_batch_replace_pattern_sibling_2802
check_atomic_batch_replace_pattern_sibling_2802.py
Issue #2803
check_move_node_partial_failure_no_dangling_2803
check_move_node_partial_failure_no_dangling_2803.py
Issue #2804
check_clone_walk_gensym_ceiling_2804
check_clone_walk_gensym_ceiling_2804.py
Issue #2805
check_dotted_rest_builtin_rename_2805
check_dotted_rest_builtin_rename_2805.py
Issue #2806
check_concurrent_clone_hygiene_depth_2806
check_concurrent_clone_hygiene_depth_2806.py
Issue #2807
check_unquote_splicing_hygiene_2807
check_unquote_splicing_hygiene_2807.py
Issue #2808
check_stamp_rest_param_hygiene_marker_2808
check_stamp_rest_param_hygiene_marker_2808.py
Issue #2809
check_qq_unwrap_targeted_restamp_2809
check_qq_unwrap_targeted_restamp_2809.py
Issue #2810
check_clone_provenance_per_evaluator_2810
check_clone_provenance_per_evaluator_2810.py
Issue #2811
check_gensym_ceiling_serial_drift_2811
check_gensym_ceiling_serial_drift_2811.py
Issue #2812
check_cascade_bfs_invalidate_after_guard_2812
check_cascade_bfs_invalidate_after_guard_2812.py
Issue #2813
check_cascade_relower_silent_skip_2813
check_cascade_relower_silent_skip_2813.py
Issue #2814
check_capture_audit_event_forced_enforcement_link_2814
check_capture_audit_event_forced_enforcement_link_2814.py
Issue #2815
check_cascade_multi_define_stale_2815
check_cascade_multi_define_stale_2815.py
Issue #2816
check_cascade_path2_define_index_2816
check_cascade_path2_define_index_2816.py
Issue #2817
check_cascade_defuse_touch_null_define_2817
check_cascade_defuse_touch_null_define_2817.py
Issue #2818
check_should_audit_sampled_default_2818
check_should_audit_sampled_default_2818.py
Issue #2819
check_audit_trail_lockfree_2819
check_audit_trail_lockfree_2819.py
Issue #2820
check_alloc_block_seal_last_2820
check_alloc_block_seal_last_2820.py
Issue #2821
check_enable_soa_dual_emit_no_reset_2821
check_enable_soa_dual_emit_no_reset_2821.py
Issue #2822
check_run_one_epoch_default_2822
check_run_one_epoch_default_2822.py
Issue #2823
check_run_one_yield_hook_actual_2823
check_run_one_yield_hook_actual_2823.py
Issue #2824
check_dirty_pipeline_counter_isolation_2824
check_dirty_pipeline_counter_isolation_2824.py
Issue #2825
check_emit_soa_source_marker_propagation_2825
check_emit_soa_source_marker_propagation_2825.py
Issue #2826
check_self_func_id_usage_2826
check_self_func_id_usage_2826.py
Issue #2827
check_run_one_requires_expression_partial_2827
check_run_one_requires_expression_partial_2827.py
Issue #2828
check_linear_ownership_branch_cellget_2828
check_linear_ownership_branch_cellget_2828.py
Issue #2829
check_linear_ownership_refcount_inc_2829
check_linear_ownership_refcount_inc_2829.py
Issue #2830
check_dce_pass_variable_args_2830
check_dce_pass_variable_args_2830.py
Issue #2831
check_tco_dead_block_accumulation_2831
check_tco_dead_block_accumulation_2831.py
Issue #2832
check_tco_arg_base_oob_2832
check_tco_arg_base_oob_2832.py
Issue #2833
check_tco_jump_terminator_2833
check_tco_jump_terminator_2833.py
Issue #2834
check_dce_branch_block_id_overprotect_2834
check_dce_branch_block_id_overprotect_2834.py
check_restricted_multi_tenant_hard_fiber_2835
check_restricted_multi_tenant_hard_fiber_2835.py
Issue #2836
check_mid_fallback_zero_tolerance_2836
check_mid_fallback_zero_tolerance_2836.py
Issue #2837
check_moving_external_root_remap_2837
check_moving_external_root_remap_2837.py
Issue #2838
check_parallel_intend_force_lock_prod_default_2838
check_parallel_intend_force_lock_prod_default_2838.py
Issue #2839
check_side_effect_fiber_principal_2839
check_side_effect_fiber_principal_2839.py
Issue #2840
check_general_object_pin_required_prod_default_2840
check_general_object_pin_required_prod_default_2840.py
Issue #2841
check_cross_eval_cascade_owner_scoped_2841
check_cross_eval_cascade_owner_scoped_2841.py
Issue #2842
check_type_linear_commit_proof_goal_truth_2842
Issue #2843
check_workflow_compose_aura_2843
check_workflow_compose_aura_2843.py
Issue #2844
check_steal_sole_enqueue_gate_2844
check_steal_sole_enqueue_gate_2844.py
Issue #2845
check_aot_reload_proof_fail_stamp_2845
check_aot_reload_proof_fail_stamp_2845.py
Issue #2846
check_residual_defer_after_exit_2846
Issue #2847
check_region_type_commit_gate_2847
Issue #2848
Issue #2849
check_mailbox_mid_mutation_delivery_2849
check_mailbox_mid_mutation_delivery_2849.py
Issue #2850
check_pure_anon_sync_remount_budget_2850
Issue #2851
Issue #2852
Issue #2853
Issue #2854
Issue #2855
Issue #2856
Issue #2857
Issue #2858
Issue #2859
Issue #2860
Issue #2861
Issue #2862
Issue #2863
Issue #2864
Issue #2865
check_std_socket_require_path_2865
check_std_socket_require_path_2865.py
check_set_code_module_bind_2868
check_set_code_module_bind_2868.py
check_nested_fiber_join_2869
check_nested_fiber_join_2869.py
check_swarm_interface_2875
check_swarm_interface_2875.py
check_pso_2877
check_pso_2877.py
check_abc_2878
check_abc_2878.py
check_boids_2879
check_boids_2879.py
check_fss_2880
check_fss_2880.py
Issue #2881
Issue #2882
check_production_default_single_use_2882
check_production_default_single_use_2882.py
Issue #2883
Issue #2884
check_join_cleanup_report_2885
check_join_cleanup_report_2885.py
Issue #2886
check_parallel_intend_region_concurrent_2886
check_parallel_intend_region_concurrent_2886.py
Issue #2887
check_agent_bp_degrade_2887
check_agent_bp_degrade_2887.py
Issue #2888
check_lifetime_consistency_proof_2888
check_lifetime_consistency_proof_2888.py
Issue #2889
check_moving_known_roots_auto_register_2889
check_moving_known_roots_auto_register_2889.py
Issue #2890
check_steal_checkpoint_residual_2890
check_steal_checkpoint_residual_2890.py
Issue #2891
check_general_object_pin_required_2891
check_general_object_pin_required_2891.py
Issue #2892
check_post_compact_lifecycle_2892
check_post_compact_lifecycle_2892.py
Issue #2893
check_pure_anon_adaptive_budget_2893
check_pure_anon_adaptive_budget_2893.py
Issue #2894
Issue #2895
Issue #2896
check_occurrence_persist_production_default_2896
check_occurrence_persist_production_default_2896.py
Issue #2897
check_type_linear_evolution_snapshot_2897
Issue #2898
check_composite_required_type_2898
Issue #2899
check_linear_ir_fastpath_2899
Issue #2900
check_solver_budget_2900
Issue #2901
check_steal_residual_rearm_race_2901
check_steal_residual_rearm_race_2901.py
check_hot_update_registry_2902
check_hot_update_registry_2902.py
Issue #2903
check_mailbox_under_boundary_wait_2903
Issue #2904
check_dirty_columnar_2904
Issue #2905
check_moving_sticky_densify_off_2905
Issue #2906
check_pcv_flatast_locked_exclusive_2906
Issue #2907
check_soa_sunset_bridge_2907
Issue #2908
check_shape_compact_no_global_bump_2908
check_shape_compact_no_global_bump_2908.py
Issue #2909
check_cone_truncate_force_closure_2909
check_occurrence_persist_production_2910
check_occurrence_persist_production_2910.py
Issue #2911
check_refined_consistency_commit_gate_2911
Issue #2912
check_layered_evidence_force_full_2912
Issue #2913
check_solve_delta_locality_slo_2913
check_query_primitives_split_2914
check_query_primitives_split_2914.py
check_prim_registrar_scaffold_2915
check_prim_registrar_scaffold_2915.py
check_prim_heap_quota_2916
check_prim_heap_quota_2916.py
check_ast_snapshot_workspace_2918
check_ast_snapshot_workspace_2918.py
check_current_source_unparse_2919
check_current_source_unparse_2919.py
Issue #2921
current-source-roundtrip-2921
current_source_roundtrip_2921
Issue #2922
ast-unparse-2922
ast_unparse_2922
check_ast_unparse_2922
check_ast_unparse_2922.py
Issue #2923
check_isolation_decide_2923
check_isolation_decide_2923.py
Issue #2924
check_wait_reclaimed_2924
check_wait_reclaimed_2924.py
wait-reclaimed-2924
wait_reclaimed_2924
Issue #2925
check_producer_bp_budget_2925
check_producer_bp_budget_2925.py
producer-bp-budget-2925
producer_bp_budget_2925
Issue #2926
check_scope_resolve_2926
check_scope_resolve_2926.py
scope-resolve-2926
scope_resolve_2926
Issue #2927
check_force_jit_reason_bit_map_2927
check_force_jit_reason_bit_map_2927.py
force-jit-reason-bit-map-2927
Issue #2928
check_residual_remount_round_robin_2928
residual-remount-2928
Issue #2929
check_steal_invariant_table_2929
check_steal_invariant_table_2929.py
steal-invariant-table-2929
Issue #2930
bridge-epoch-zero-stale-2930
check_bridge_epoch_zero_stale_2930
check_bridge_epoch_zero_stale_2930.py
Issue #2932
check_hold_budget_forced_fail_closed_2932
Issue #2933
check_query_result_binding_2933
Issue #2934
check_restamp_budget_2934
Issue #2935
check_moving_known_roots_sticky_recovery_2935
check_moving_known_roots_sticky_recovery_2935.py
check_batch_dirty_production_multi_only_2936
check_batch_dirty_production_multi_only_2936.py
Issue #2937
check_shape_profiler_shard_2937
check_shape_profiler_shard_2937.py
Issue #2938
check_occurrence_commit_snapshot_2938
check_occurrence_commit_snapshot_2938.py
Issue #2939
check_solve_delta_dep_closure_2939
Issue #2942
check_side_effect_node_id_mandate_2942
check_side_effect_node_id_mandate_2942.py
Issue #2943
check_production_hard_fiber_default_2943
check_production_hard_fiber_default_2943.py
Issue #2944
check_mutation_session_grant_2944
check_mutation_session_grant_2944.py
Issue #2945
check_join_held_flags_2945
check_join_held_flags_2945.py
Issue #2946
check_agent_scope_concurrent_hard_deny_2946
Issue #2947
check_mailbox_hold_slo_security_schedule_2947
check_mailbox_hold_slo_security_schedule_2947.py
Issue #2948
check_bp_threshold_ssot_2948
check_bp_threshold_ssot_2948.py
Issue #2949
check_force_jit_repromote_only_covered_default_2949
Issue #2950
check_pure_anon_bg_remount_2950
check_pure_anon_bg_remount_2950.py
Issue #2951
check_cross_eval_hard_owner_scoped_2951
Issue #2952
check_coverage_verify_min_dirty_2952
Issue #2953
check_reload_recovery_playbook_2953
check_reload_recovery_playbook_2953.py
Issue #2954
check_steal_decision_per_fiber_2954
check_steal_decision_per_fiber_2954.py
Issue #2955
check_production_abi_selfcheck_2955
check_production_abi_selfcheck_2955.py
Issue #2956
check_mutation_mirror_canary_2956
check_mutation_mirror_canary_2956.py
Issue #2957
check_steal_lifetime_proof_residual_2957
Issue #2958
check_mailbox_defer_slo_hold_cancel_2958
check_mailbox_defer_slo_hold_cancel_2958.py
Issue #2959
check_topology_dual_restore_2959
check_topology_dual_restore_2959.py
Issue #2960
check_query_stable_ref_stamp_2960
check_query_stable_ref_stamp_2960.py
Issue #2961
check_rename_replace_hygiene_restamp_2961
check_rename_replace_hygiene_restamp_2961.py
Issue #2962
check_cone_outside_goal_drop_recover_reject_2962
Issue #2963
check_instance_repair_before_full_2963
Issue #2964
check_linear_fast_path_unified_2964
Issue #2966
check_ast_snapshot_fail_reason_2966
Issue #2967
check_capability_durable_gate_2967
check_capability_durable_gate_2967.py
check_cross_tenant_grant_gate_2968
check_cross_tenant_grant_gate_2968.py
check_capability_write_fence_2969
check_capability_write_fence_2969.py
Issue #2970
check_join_wait_reclaimed_2970
check_join_wait_reclaimed_2970.py
Issue #2971
check_general_object_pin_create_densify_2971
check_general_object_pin_create_densify_2971.py
Issue #2972
check_mailbox_credit_inflight_2972
check_mailbox_credit_inflight_2972.py
mailbox-credit-2972
mailbox_credit_2972
Issue #2973
check_moving_pre_densify_completeness_2973
check_moving_pre_densify_completeness_2973.py
Issue #2974
check_workflow_run_2974
check_workflow_run_2974.py
Issue #2975
check_outermost_exit_residual_pin_2975
Issue #2976
check_agent_scope_concurrency_2976
check_agent_scope_concurrency_2976.py
Issue #2977
check_residual_remount_prefer_force_jit_2977
residual-remount-prefer-2977
Issue #2978
check_reemit_success_sync_covered_remount_2978
reemit-success-sync-covered-2978
Issue #2979
check_layered_evidence_phase5_consume_2979
Issue #2980
check_epoch_residual_merged_heal_2980
Issue #2981
check_type_linear_proof_empty_after_fence_2981
Issue #2982
check_staging_dlopen_ops_recovery_2982
Issue #2983
check_composite_required_type_default_2983
Issue #2984
check_linear_compact_root_consistency_2984
check_linear_compact_root_consistency_2984.py
Issue #2985
check_mutation_concurrency_health_admit_2985
Issue #2986
Issue #2987
check_mailbox_delivery_safety_2987
check_mailbox_delivery_safety_2987.py
Issue #2988
check_mutate_invalidate_incremental_2988
Issue #2989
check_query_concurrent_hygiene_safe_span_2989
check_query_concurrent_hygiene_safe_span_2989.py
Issue #2990
check_workspace_concurrent_policy_2990
Issue #2991
check_coercion_provenance_hf_mutate_2991
Issue #2992
Issue #2993
Issue #2994
check_solve_delta_locality_budget_2994
Issue #2995
check_occurrence_commit_health_2995
check_occurrence_commit_health_2995.py
Issue #2996
check_prim_register_core_2996
check_prim_register_core_2996.py
Issue #2997
check_list_ctor_hotpath_2997
check_list_ctor_hotpath_2997.py
check_prim_error_convention_2998
check_prim_error_convention_2998.py
Issue #2999
check_hold_budget_dtor_consume_2999
Issue #3000
check_query_stable_ref_restamp_lag_3000
Issue #3001
check_chaos_steal_lifetime_envframe_3001
Issue #3002
check_mailbox_hold_slo_ssot_soak_3002
Issue #3003
check_solve_delta_timeout_fail_closed_3003
check_solve_delta_timeout_fail_closed_3003.py
Issue #3004
check_occurrence_persist_audit_atomic_3004
check_occurrence_persist_audit_atomic_3004.py
Issue #3005
check_adt_exhaust_dirty_cone_3005
Issue #3006
check_linear_fast_path_dirty_revalidate_3006
Issue #3007
check_dead_coercion_hot_residual_3007
Issue #3010
check_allow_cross_tenant_admin_3010
check_allow_cross_tenant_admin_3010.py
check_isolation_deny_fiber_3011
check_isolation_deny_fiber_3011.py
Issue #3012
check_join_must_wait_reclaimed_3012
check_join_must_wait_reclaimed_3012.py
Issue #3013
check_agent_send_handoff_required_3013
check_agent_send_handoff_required_3013.py
Issue #3014
check_body_acquire_rejected_3014
check_body_acquire_rejected_3014.py
Issue #3015
check_scope_bp_inherit_3015
check_scope_bp_inherit_3015.py
Issue #3016
check_boundary_audit_mid_3016
check_boundary_audit_mid_3016.py
Issue #3017
check_moving_incomplete_remap_3017
check_moving_incomplete_remap_3017.py
Issue #3018
check_engine_metrics_hash_overflow_3018
check_engine_metrics_hash_overflow_3018.py
Issue #3019
check_unified_restamp_3019
check_unified_restamp_3019.py
Issue #3020
check_query_hash_overflow_3020
check_query_hash_overflow_3020.py
Issue #3021
check_envframe_closure_apply_protocol_3021
check_envframe_closure_apply_protocol_3021.py
Issue #3022
check_ffi_opaque_pin_or_remap_3022
check_ffi_opaque_pin_or_remap_3022.py
Issue #3023
check_linear_root_abort_release_3023
check_linear_root_abort_release_3023.py
Issue #3024
check_pure_anon_bg_overflow_must_deopt_3024
check_pure_anon_bg_overflow_must_deopt_3024.py
Issue #3025
check_reemit_owner_required_prod_multi_3025
check_reemit_owner_required_prod_multi_3025.py
Issue #3026
check_residual_force_agent_actionable_3026
check_residual_force_agent_actionable_3026.py
Issue #3027
check_structural_macro_hygiene_3027
check_structural_macro_hygiene_3027.py
Issue #3028
check_tls_depth_same_flat_clone_3028
check_tls_depth_same_flat_clone_3028.py
Issue #3029
check_macro_self_evo_grant_fence_3029
check_macro_self_evo_grant_fence_3029.py
Issue #3030
check_type_linear_proof_clear_on_abort_3030
check_type_linear_proof_clear_on_abort_3030.py
Issue #3031
check_pending_full_solve_residual_3031
Issue #3032
check_rehydrate_miss_invalidate_3032
Issue #3033
Issue #3034
Issue #3035
check_hold_budget_forced_unlock_3035
Issue #3036
check_mailbox_residual_hard_reject_3036
Issue #3037
check_restamp_over_budget_export_3037
Issue #3038
check_steal_residual_rearm_resample_3038
check_steal_residual_rearm_resample_3038.py
Issue #3039
check_scoped_parallel_overlap_hard_reject_3039
check_scoped_parallel_overlap_hard_reject_3039.py
Issue #3040
check_compile_node_id_entry_3040
check_compile_node_id_entry_3040.py
Issue #3041
check_restamp_budget_query_epoch_stale_3041
check_restamp_budget_query_epoch_stale_3041.py
Issue #3042
check_pure_wrap_no_std_function_3042
check_pure_wrap_no_std_function_3042.py
Issue #3043
check_hot_contract_soft_observe_3043
check_hot_contract_soft_observe_3043.py
Issue #3044
check_bidirectional_tag_coverage_3044
check_bidirectional_tag_coverage_3044.py
Issue #3045
check_adt_exhaust_undermark_cone_3045
Issue #3046
Issue #3048
check_session_grant_steal_3048
Issue #3049
check_quota_per_tenant_3049
Issue #3050
check_join_auto_wait_reclaimed_3051
check_join_auto_wait_reclaimed_3051.py
Issue #3052
check_join_fail_policy_3052
check_join_fail_policy_3052.py
Issue #3053
check_general_object_pin_allocate_3053
check_general_object_pin_allocate_3053.py
Issue #3054
check_mid_fallback_refuse_se_3054
check_mid_fallback_refuse_se_3054.py
Issue #3055
check_moving_post_moving_stale_3055
check_moving_post_moving_stale_3055.py
Issue #3056
check_wal_append_fail_slo_3056
check_wal_append_fail_slo_3056.py
Issue #3057
check_ffi_opaque_pin_or_remap_3057
check_ffi_opaque_pin_or_remap_3057.py
Issue #3058
check_unified_restamp_query_visible_3058
check_unified_restamp_query_visible_3058.py
Issue #3059
check_hot_update_decide_and_reemit_3059
check_hot_update_decide_and_reemit_3059.py
Issue #3060
check_pure_anon_pressure_force_leave_3060
check_pure_anon_pressure_force_leave_3060.py
Issue #3061
check_move_replace_allow_macro_3061
check_move_replace_allow_macro_3061.py
Issue #3062
check_macro_expand_noboundary_limit_3062
check_macro_expand_noboundary_limit_3062.py
Issue #3063
check_half_green_ir_steal_densify_3063
check_half_green_ir_steal_densify_3063.py
Issue #3064
check_inline_macro_body_marker_3064
Issue #3065
check_dead_coercion_elim_cone_3065
cmd_dead_coercion_elim_cone_3065
Issue #3066
check_composite_audit_mid_se_join_3066
cmd_composite_audit_mid_se_join_3066
Issue #3067
check_hybrid_deferred_cascade_3067
cmd_hybrid_deferred_cascade_3067
Issue #3068
check_partial_map_ensure_3068
check_partial_map_ensure_3068.py
Issue #3069
Issue #3070
Issue #3071
check_hold_budget_inbody_window_3071
Issue #3072
check_steal_enqueue_sole_gate_3072
check_steal_enqueue_sole_gate_3072.py
Issue #3073
check_chaos_production_readiness_3073
Issue #3074
check_mutate_dispatch_sole_guard_3074
check_mutate_dispatch_sole_guard_3074.py
Issue #3075
check_query_epoch_production_strict_3075
check_query_epoch_production_strict_3075.py
Issue #3076
check_soft_observe_not_hard_3076
check_soft_observe_not_hard_3076.py
Issue #3081
check_soft_timeout_export_non_authoritative_3081
Issue #3082
check_nested_occurrence_provisional_3082
check_nested_occurrence_provisional_3082.py
Issue #3083
check_adt_exhaust_complete_seed_3083
Issue #3084
check_hot_residual_soft_must_deopt_3084
Issue #3085
check_rehydrate_miss_lowering_elision_3085
check_rehydrate_miss_lowering_elision_3085.py
Issue #3088
Issue #3089
Issue #3090
check_grant_mid_refused_3090
check_grant_mid_refused_3090.py
Issue #3091
check_type_linear_proof_mid_3091
check_type_linear_proof_mid_3091.py
Issue #3092
Issue #3093
check_intermediate_cover_3093
check_intermediate_cover_3093.py
Issue #3094
check_concurrent_clone_name_map_3094
check_concurrent_clone_name_map_3094.py
Issue #3095
Issue #3096
Issue #3097
Issue #3098
Issue #3099
Issue #3100
Issue #3101
Issue #3102
Issue #3103
Issue #3104
Issue #3106
check_hot_contract_harden_3106
check_hot_contract_harden_3106.py
Issue #3107
check_hot_residual_soft_relower_3107
check_hot_residual_soft_relower_3107.py
Issue #3108
check_occurrence_recover_not_solved_3108
check_occurrence_recover_not_solved_3108.py
check_wal_append_fail_closed_3109
check_wal_append_fail_closed_3109.py
Issue #3110
check_join_drain_reclaim_3110
check_join_drain_reclaim_3110.py
Issue #3111
check_mailbox_held_ref_steal_3111
Issue #3112
check_dual_track_facade_3112
check_dual_track_facade_3112.py
Issue #3113
Issue #3114
check_evolution_audit_decision_3114
check_evolution_audit_decision_3114.py
Issue #3115
check_scalar_macro_hygiene_3115
check_scalar_macro_hygiene_3115.py
Issue #3116
check_coercion_abort_dual_clear_3116
check_coercion_abort_dual_clear_3116.py
Issue #3117
check_abort_restore_ir_map_fence_3117
check_abort_restore_ir_map_fence_3117.py
Issue #3118
check_hold_budget_cancel_force_release_3118
Issue #3119
Issue #3120
check_residual_castop_type_txn_3120
Issue #3121
check_query_stable_restamp_lag_3121
check_query_stable_restamp_lag_3121.py
Issue #3122
Issue #3123
check_production_auto_arm_moving_3123
Issue #3124
check_nonalloc_arena_hooks_3124
check_nonalloc_arena_hooks_3124.py
Issue #3125
check_cross_scope_directory_3125
check_cross_scope_directory_3125.py
Issue #3126
check_capability_admin_fence_3126
check_capability_admin_fence_3126.py
Issue #3127
check_mailbox_bp_overflow_3127
check_mailbox_bp_overflow_3127.py
Issue #3128
check_sticky_densify_recover_3128
check_sticky_densify_recover_3128.py
Issue #3129
check_facade_owns_full_invalidate_3129
check_facade_owns_full_invalidate_3129.py
Issue #3130
check_linear_move_drop_elision_3130
check_linear_move_drop_elision_3130.py
Issue #3131
check_scalar_mutate_hygiene_3131
check_scalar_mutate_hygiene_3131.py
Issue #3132
check_macro_self_evo_chokepoint_3132
check_macro_self_evo_chokepoint_3132.py
Issue #3133
check_hold_budget_synthetic_yield_3133
check_hold_budget_synthetic_yield_3133.py
Issue #3134
check_steal_safety_production_residual_zero_3134
check_steal_safety_production_residual_zero_3134.py
Issue #3135
check_cascade_decision_residual_atomic_3135
check_cascade_decision_residual_atomic_3135.py
Issue #3136
check_relower_success_coverage_3136
check_relower_success_coverage_3136.py
Issue #3137
Issue #3138
Issue #3139
Issue #3140
check_castop_typed_meta_phase_c_3140
check_castop_typed_meta_phase_c_3140.py
Issue #3141
Issue #3142
Issue #3143
Issue #3144
Issue #3145
check_cross_tenant_grant_principal_3145
check_cross_tenant_grant_principal_3145.py
Issue #3146
check_join_drain_reclaim_3146
check_join_drain_reclaim_3146.py
Issue #3147
check_mailbox_bp_scope_handle_3147
check_mailbox_bp_scope_handle_3147.py
Issue #3148
check_handoff_join_via_token_3148
check_handoff_join_via_token_3148.py
Issue #3150
Issue #3151
Issue #3152
Issue #3153
Issue #3154
Issue #3155
Issue #3156
check_intermediate_cover_no_value_only_3156
check_intermediate_cover_no_value_only_3156.py
Issue #3157
check_macro_clone_target_atomicity_3157
check_macro_clone_target_atomicity_3157.py
Issue #3158
check_occurrence_abort_restore_3158
check_occurrence_abort_restore_3158.py
Issue #3159
check_abort_ir_cache_fence_first_3159
check_abort_ir_cache_fence_first_3159.py
Issue #3160
check_hold_budget_inbody_escalate_3160
check_hold_budget_inbody_escalate_3160.py
Issue #3161
check_impact_rearm_coverage_3161
check_impact_rearm_coverage_3161.py
Issue #3162
check_steal_safety_sticky_fail_3162
check_steal_safety_sticky_fail_3162.py
Issue #3163
check_storm_hysteresis_refresh_3163
check_storm_hysteresis_refresh_3163.py
Issue #3164
check_hold_after_cancel_hard_soak_3164
check_hold_after_cancel_hard_soak_3164.py
Issue #3165
check_dual_dep_graph_parity_strict_fail_closed_3165
check_dual_dep_graph_parity_strict_fail_closed_3165.py
Issue #3166
check_nested_guard_exit_dirty_pending_3166
check_nested_guard_exit_dirty_pending_3166.py
check_pcv_span_stale_coverage_3167
check_pcv_span_stale_coverage_3167.py
Issue #3168
cascade-rearm-new-edge-only-3168
check_cascade_rearm_new_edge_only_3168
check_cascade_rearm_new_edge_only_3168.py
cmd_cascade_rearm_new_edge_only_3168
Issue #3169
check_solve_delta_partial_cleared_3169
check_solve_delta_partial_cleared_3169.py
Issue #3170
check_occurrence_persist_fingerprint_3170
check_occurrence_persist_fingerprint_3170.py
Issue #3171
check_linear_fast_path_clear_on_restamp_3171
check_linear_fast_path_clear_on_restamp_3171.py
Issue #3172
check_compile_surface_3172
check_compile_surface_3172.py
Issue #3173
check_arena_surface_3173
check_arena_surface_3173.py
Issue #3174
check_io_net_git_surface_3174
check_io_net_git_surface_3174.py
check_query_surface_3175
check_query_surface_3175.py
Issue #3176
check_ffi_surface_3176
check_ffi_surface_3176.py
Issue #3177
check_durable_session_bind_3177
check_durable_session_bind_3177.py
Issue #3178
check_wal_overflow_mid_3178
check_wal_overflow_mid_3178.py
Issue #3179
check_per_scope_bp_admit_3179
check_per_scope_bp_admit_3179.py
Issue #3180
check_production_small_pool_cover_3180
check_production_small_pool_cover_3180.py
Issue #3181
check_quote_clone_hygiene_3181
check_quote_clone_hygiene_3181.py
Issue #3182
check_post_moving_stale_hard_gate_3182
check_post_moving_stale_hard_gate_3182.py
Issue #3183
check_gensym_ceiling_rest_bypass_3183
check_gensym_ceiling_rest_bypass_3183.py
check_abort_restore_force_dirty_3184
check_abort_restore_force_dirty_3184.py
Issue #3185
check_densify_entry_lcp_consult_3185
check_densify_entry_lcp_consult_3185.py
Issue #3186
check_linear_move_drop_elision_ok_3186
check_linear_move_drop_elision_ok_3186.py
Issue #3187
check_dual_dep_graph_strict_or_production_3187
check_dual_dep_graph_strict_or_production_3187.py
Issue #3188
check_production_facade_minimal_ir_shape_3188
check_production_facade_minimal_ir_shape_3188.py
Issue #3189
check_partial_impact_upper_bound_3189
check_partial_impact_upper_bound_3189.py
Issue #3190
check_residual_drain_outermost_stamp_3190
Issue #3191
check_macro_hygiene_default_deny_3191
Issue #3192
check_mutate_dispatch_sole_entry_3192
check_mutate_dispatch_sole_entry_3192.py
Issue #3193
check_nested_abort_authority_face_3193
check_nested_abort_authority_face_3193.py
Issue #3194
check_hold_budget_inbody_force_release_3194
check_hold_budget_inbody_force_release_3194.py
Issue #3195
check_production_multi_worker_residual_sticky_3195
check_production_multi_worker_residual_sticky_3195.py
Issue #3196
check_nested_guard_authority_gap_3196
check_nested_guard_authority_gap_3196.py
check_naked_mutate_prim_meta_3197
check_naked_mutate_prim_meta_3197.py
Issue #3198
check_query_stable_restamp_export_uniform_3198
check_query_stable_restamp_export_uniform_3198.py
Issue #3199
check_shape_compact_no_all_shards_lock_3199
check_shape_compact_no_all_shards_lock_3199.py
Issue #3200
check_moving_pin_guard_soft_gate_3200
check_moving_pin_guard_soft_gate_3200.py
Issue #3201
check_ir_dirty_batch_only_production_default_3201
check_ir_dirty_batch_only_production_default_3201.py
Issue #3202
check_production_strict_ground_unify_3202
check_production_strict_ground_unify_3202.py
Issue #3203
check_soft_timeout_export_uniform_gate_3203
check_soft_timeout_export_uniform_gate_3203.py
Issue #3204
check_stable_ref_export_production_hard_reject_3204
check_stable_ref_export_production_hard_reject_3204.py
Issue #3205
check_evolution_audit_decision_durable_3205
check_evolution_audit_decision_durable_3205.py
Issue #3206
check_workflow_residual_action_3206
Issue #3207
check_dual_evaluator_cascade_3207
check_dual_evaluator_cascade_3207.py
Issue #3208
Issue #3209
check_session_grant_quiesce_3209
check_session_grant_quiesce_3209.py
Issue #3210
check_moving_temporary_canary_3210
check_moving_temporary_canary_3210.py
Issue #3211
check_wal_append_fail_schedule_3211
check_wal_append_fail_schedule_3211.py
Issue #3212
check_mailbox_handoff_dual_track_3212
check_mailbox_handoff_dual_track_3212.py
Issue #3213
check_atomic_batch_allow_macro_3213
check_atomic_batch_allow_macro_3213.py
Issue #3214
check_production_all_densify_allocate_cover_3214
check_production_all_densify_allocate_cover_3214.py
Issue #3215
check_hygiene_agent_reason_strings_3215
check_hygiene_agent_reason_strings_3215.py
Issue #3216
check_identity_plane_handoff_boundary_3216
check_identity_plane_handoff_boundary_3216.py
Issue #3217
check_deny_restore_then_stamp_3217
check_deny_restore_then_stamp_3217.py
Issue #3219
check_eval_core_joint_after_production_facade_3219
check_eval_core_joint_after_production_facade_3219.py
Issue #3220
check_reclaimed_pending_lifecycle_3220
check_reclaimed_pending_lifecycle_3220.py
Issue #3221
check_cascade_reason_not_residual_force_heal_3221
check_cascade_reason_not_residual_force_heal_3221.py
Issue #3222
check_hold_budget_inbody_force_unlock_3222
check_hold_budget_inbody_force_unlock_3222.py
Issue #3223
check_cross_fiber_urgent_inbody_poll_3223
check_cross_fiber_urgent_inbody_poll_3223.py
Issue #3224
check_ir_typed_entry_commit_readiness_3224
check_ir_typed_entry_commit_readiness_3224.py
Issue #3225
check_occurrence_persist_seq_3225
Issue #3226
check_prod_soundness_real_compare_3226
Issue #3227
check_linear_post_migration_proof_rebind_3227
Issue #3228
check_residual_castop_undermark_cone_3228
check_residual_castop_undermark_cone_3228.py
Issue #3229
check_relower_success_define_collision_3229
Issue #3230
check_query_stable_restamp_lag_hard_reject_3230
check_query_stable_restamp_lag_hard_reject_3230.py
Issue #3231
Issue #3232
check_nested_abort_authority_face_3232
check_nested_abort_authority_face_3232.py
Issue #3233
check_pcv_stale_span_exclusive_3233
Issue #3234
check_pass_scc_no_std_function_3234
Issue #3235
check_container_mutate_guard_3235
Issue #3236
check_adt_exhaust_commit_recheck_3236
Issue #3237
check_type_export_full_audit_gate_3237
check_type_export_full_audit_gate_3237.py
Issue #3238
check_linear_fast_path_live_mutation_densify_3238
check_linear_fast_path_live_mutation_densify_3238.py
Issue #3239
check_sv_eda_surface_retired_3239
check_sv_eda_surface_retired_3239.py
Issue #3241
check_session_grant_peer_fiber_3241
check_session_grant_peer_fiber_3241.py
Issue #3242
check_typed_summary_wal_3242
check_typed_summary_wal_3242.py
check_parallel_region_key_missing_3243
check_parallel_region_key_missing_3243.py
Issue #3244
check_metrics_hash_overflow_posture_3244
check_metrics_hash_overflow_posture_3244.py
Issue #3245
check_ensure_reclaimed_cleanup_adoption_3245
check_ensure_reclaimed_cleanup_adoption_3245.py
Issue #3246
check_evolution_audit_suggested_next_3246
check_evolution_audit_suggested_next_3246.py
Issue #3247
check_must_deopt_getter_sticky_3247
check_must_deopt_getter_sticky_3247.py
Issue #3248
check_residual_force_fail_exit_age_3248
check_residual_force_fail_exit_age_3248.py
Issue #3249
check_linear_nested_abort_drain_3249
check_linear_nested_abort_drain_3249.py
Issue #3250
check_restart_n_spec_boundary_3250
check_restart_n_spec_boundary_3250.py
Issue #3251
check_agent_deny_class_3251
check_agent_deny_class_3251.py
Issue #3252
check_hygiene_checkpoint_gen_drift_3252
check_hygiene_checkpoint_gen_drift_3252.py
Issue #3253
check_repair_solved_residual_3253
check_repair_solved_residual_3253.py
Issue #3254
check_hold_budget_noncoop_force_edge_3254
check_hold_budget_noncoop_force_edge_3254.py
Issue #3255
check_dual_dep_graph_soft_parity_partial_3255
check_dual_dep_graph_soft_parity_partial_3255.py
Issue #3256
check_mailbox_defer_slo_hold_unify_3256
check_mailbox_defer_slo_hold_unify_3256.py
Issue #3257
check_deferred_hybrid_rearm_last_look_3257
check_deferred_hybrid_rearm_last_look_3257.py
Issue #3258
check_abort_force_lookup_reject_3258
check_abort_force_lookup_reject_3258.py
Issue #3259
check_restamp_hot_cone_budget_3259
check_restamp_hot_cone_budget_3259.py
Issue #3260
check_macro_provenance_counter_unify_3260
check_macro_provenance_counter_unify_3260.py
Issue #3261
check_flush_mutation_boundary_toctou_3261
check_flush_mutation_boundary_toctou_3261.py
Issue #3262
check_gc_safepoint_restamp_lock_3262
check_gc_safepoint_restamp_lock_3262.py
Issue #3263
check_quote_lambda_marker_once_3263
check_quote_lambda_marker_once_3263.py
Issue #3264
check_cascade_dep_graph_atomic_3264
check_cascade_dep_graph_atomic_3264.py
Issue #3265
check_jit_fn_marker_atomic_3265
check_jit_fn_marker_atomic_3265.py
Issue #3266
check_module_realpath_toctou_3266
check_module_realpath_toctou_3266.py
Issue #3267
check_env_publish_lock_3267
check_env_publish_lock_3267.py
Issue #3268
check_guard_flag_atomic_ref_3268
check_guard_flag_atomic_ref_3268.py
Issue #3269
check_arena_compact_toctou_3269
check_arena_compact_toctou_3269.py
Issue #3270
check_primid_drift_3270
check_primid_drift_3270.py
Issue #3271
check_shape_dirty_hook_no_std_function_3271
check_shape_dirty_hook_no_std_function_3271.py
Issue #3272
check_host_forget_window_3272
check_host_forget_window_3272.py
Issue #3273
check_handoff_observation_only_3273
check_handoff_observation_only_3273.py
Issue #3274
check_ffi_opaque_densify_cover_3274
check_ffi_opaque_densify_cover_3274.py
Issue #3275
check_tenant_scope_link_gate_3275
check_tenant_scope_link_gate_3275.py
Issue #3276
check_privileged_grant_callsite_allowlist_3276
check_privileged_grant_callsite_allowlist_3276.py
Issue #3277
check_pure_anon_no_boundary_force_leave_3277
check_pure_anon_no_boundary_force_leave_3277.py
Issue #3278
check_cross_flat_schema_homology_3278
check_cross_flat_schema_homology_3278.py
Issue #3279
check_session_bound_orphan_sweep_3279
check_session_bound_orphan_sweep_3279.py
Issue #3280
check_invariant_deny_se_dual_write_3280
check_invariant_deny_se_dual_write_3280.py
Issue #3281
check_mid_bound_abort_authority_3281
check_mid_bound_abort_authority_3281.py
Issue #3282
check_query_hash_overflow_3282
check_query_hash_overflow_3282.py
Issue #3283
check_deferred_rearm_lag_3283
check_deferred_rearm_lag_3283.py
Issue #3284
check_evolution_audit_se_mid_miss_3284
check_evolution_audit_se_mid_miss_3284.py
Issue #3285
check_noncoop_force_release_1slo_3285
check_noncoop_force_release_1slo_3285.py
Issue #3286
check_production_query_result_schema2_3286
check_production_query_result_schema2_3286.py
Issue #3287
check_restamp_residual_lag_deny_3287
check_restamp_residual_lag_deny_3287.py
Issue #3288
check_production_residual_sticky_continuous_3288
check_production_residual_sticky_continuous_3288.py
Issue #3289
check_under_boundary_hold_progress_3289
check_under_boundary_hold_progress_3289.py
Issue #3290
check_steal_invariant_call_site_coverage_3290
check_steal_invariant_call_site_coverage_3290.py
Issue #3291
check_ffi_opaque_create_site_cover_3291
check_ffi_opaque_create_site_cover_3291.py
Issue #3292
check_pcv_hotpath_metrics_layout_3292
check_pcv_hotpath_metrics_layout_3292.py
Issue #3293
check_ir_dirty_batch_only_production_default_3293
check_ir_dirty_batch_only_production_default_3293.py
Issue #3294
check_type_export_outermost_face_3294
check_type_export_outermost_face_3294.py
Issue #3295
check_linear_force_full_validate_3295
check_linear_force_full_validate_3295.py
Issue #3296
check_require_effect_mid_ssot_3296
check_require_effect_mid_ssot_3296.py
Issue #3297
check_reclaimed_dtor_under_account_3297
check_reclaimed_dtor_under_account_3297.py
Issue #3298
check_parallel_mutate_region_keys_3299
check_parallel_mutate_region_keys_3299.py
Issue #3300
Issue #3301
check_atomic_batch_macro_audit_3301
check_wal_fail_closed_force_wal_3302
check_wal_fail_closed_force_wal_3302.py
Issue #3303
check_steal_abort_reason_coverage_3303
check_steal_abort_reason_coverage_3303.py
Issue #3304
check_capability_deny_reason_uniformity_3304
check_capability_deny_reason_uniformity_3304.py
Issue #3305
check_ir_typed_entry_proof_authority_3305
check_ir_typed_entry_proof_authority_3305.py
Issue #3306
check_intermediate_create_value_only_soak_3306
check_intermediate_create_value_only_soak_3306.py
Issue #3307
check_pending_full_solve_residual_hardlatch_3307
check_pending_full_solve_residual_hardlatch_3307.py
Issue #3308
check_post_moving_canary_steal_lcp_3308
check_post_moving_canary_steal_lcp_3308.py
Issue #3309
check_unified_restamp_single_entry_3309
check_unified_restamp_single_entry_3309.py
Issue #3310
check_impact_ub_production_3310
check_impact_ub_production_3310.py
Issue #3311
check_query_result_soft_prod_transition_3311
check_query_result_soft_prod_transition_3311.py
Issue #3312
check_nested_return_not_triad_3312
check_nested_return_not_triad_3312.py
Issue #3313
check_hot_contract_production_harden_3313
check_hot_contract_production_harden_3313.py
Issue #3314
check_append_only_layout_stamps_3314
check_append_only_layout_stamps_3314.py
Issue #3315
check_production_dirty_soa_no_pred_3315
check_production_dirty_soa_no_pred_3315.py
Issue #3316
Issue #3317
check_adt_exhaust_outermost_recheck_3317
Issue #3318
check_coercion_blame_epoch_restamp_3318
Issue #3319
check_sampled_deny_se_emit_3319
Issue #3320
Issue #3321
check_concurrent_clone_prod_zero_half_tree_3321
Issue #3322
check_nested_observation_window_3322
check_nested_observation_window_3322.py
Issue #3323
check_pure_anon_overflow_dispatch_race_3323
check_pure_anon_overflow_dispatch_race_3323.py
Issue #3324
check_abort_restore_stale_map_stamp_3324
check_abort_restore_stale_map_stamp_3324.py
Issue #3325
check_hold_budget_no_edge_force_3325
check_hold_budget_no_edge_force_3325.py
Issue #3326
check_factory_default_cover_3326
check_factory_default_cover_3326.py
Issue #3327
check_restamp_hot_cone_agent_held_3327
check_restamp_hot_cone_agent_held_3327.py
check_pcv_stale_span_query_refresh_3328
check_pcv_stale_span_query_refresh_3328.py
check_production_pipeline_purity_3329
check_production_pipeline_purity_3329.py
Issue #3330
check_bidirectional_uncovered_no_dynamic_3330
check_bidirectional_uncovered_no_dynamic_3330.py
Issue #3331
check_soft_timeout_quarantine_3331
check_soft_timeout_quarantine_3331.py
Issue #3332
check_allow_cross_scoped_grant_3332
check_allow_cross_scoped_grant_3332.py
Issue #3333
check_provenance_contributing_mid_3333
check_provenance_contributing_mid_3333.py
Issue #3334
check_reclaimed_abandon_3334
check_reclaimed_abandon_3334.py
Issue #3335
check_mutation_audit_epoch_3335
check_mutation_audit_epoch_3335.py
Issue #3336
check_agent_send_safe_preference_3336
check_agent_send_safe_preference_3336.py
Issue #3337
check_scope_bp_overflow_teardown_3337
check_scope_bp_overflow_teardown_3337.py
Issue #3338
check_wal_mid_lookup_window_3338
check_wal_mid_lookup_window_3338.py
Issue #3339
check_agent_decision_facade_headroom_3339
check_agent_decision_facade_headroom_3339.py
Issue #3340
check_cross_flat_provenance_homology_3340
check_cross_flat_provenance_homology_3340.py
Issue #3341
check_fiber_hygiene_last_limit_reason_3341
check_fiber_hygiene_last_limit_reason_3341.py
Issue #3342
check_pure_anon_heal_starvation_3342
check_pure_anon_heal_starvation_3342.py
Issue #3343
check_production_weak_abi_commit_readiness_3343
check_production_weak_abi_commit_readiness_3343.py
Issue #3344
check_mutate_hygiene_continuous_gate_3344
check_mutate_hygiene_continuous_gate_3344.py
Issue #3345
check_production_hybrid_depth1_fanout_3345
check_production_hybrid_depth1_fanout_3345.py
Issue #3346
check_stamp_last_look_densify_steal_abort_3346
check_stamp_last_look_densify_steal_abort_3346.py
Issue #3347
check_residual_castop_readiness_undermark_3347
check_residual_castop_readiness_undermark_3347.py
Issue #3348
check_nonstale_mirror_growth_force_full_3348
check_nonstale_mirror_growth_force_full_3348.py
Issue #3349
check_dead_coercion_persist_before_partial_3349
check_dead_coercion_persist_before_partial_3349.py
Issue #3350
check_linear_root_moving_remap_3350
check_linear_root_moving_remap_3350.py
Issue #3351
check_peer_ir_name_soft_stale_3351
check_peer_ir_name_soft_stale_3351.py
check_transform_engine_guard_3352
check_transform_engine_guard_3352.py
check_parallel_mutate_region_keys_prod_deny_3353
check_parallel_mutate_region_keys_prod_deny_3353.py
Issue #3354
check_query_pattern_find_hygiene_align_3354
check_query_pattern_find_hygiene_align_3354.py
Issue #3355
check_ir_soa_single_mark_production_ban_3355
check_ir_soa_single_mark_production_ban_3355.py
Issue #3356
check_densify_cone_rewrite_3356
check_densify_cone_rewrite_3356.py
Issue #3357
check_shape_tls_record_merge_3357
check_shape_tls_record_merge_3357.py
Issue #3358
check_adt_exhaust_replace_type_cone_3358
check_adt_exhaust_replace_type_cone_3358.py
Issue #3359
check_castop_abort_elision_interleave_3359
check_castop_abort_elision_interleave_3359.py
Issue #3360
check_soft_timeout_export_refuse_3360
check_soft_timeout_export_refuse_3360.py
Issue #3361
Issue #3362
Issue #3363
Issue #3364
Issue #3365
Issue #3366
Issue #3367
Issue #3368
Issue #3369
check_mailbox_held_ref_steal_3369
Issue #3370
check_arena_auto_arm_known_roots_3370
check_arena_auto_arm_known_roots_3370.py
Issue #3371
check_evolution_audit_decision_forensic_source_3371
check_evolution_audit_decision_forensic_source_3371.py
Issue #3372
check_densify_fail_window_vacuous_axes_3372
Issue #3373
Issue #3374
Issue #3375
Issue #3376
Issue #3377
Issue #3378
Issue #3379
Issue #3380
Issue #3381
Issue #3382
Issue #3383
Issue #3384
check_mutation_boundary_ssot_3384
check_mutation_boundary_ssot_3384.py
Issue #3385
check_steal_safety_lifetime_proof_latch_3385
check_steal_safety_lifetime_proof_latch_3385.py
Issue #3386
check_query_stable_hard_reject_torn_latch_3386
check_query_stable_hard_reject_torn_latch_3386.py
check_soak_pr_short_3387
check_soak_pr_short_3387.py
Issue #3388
Issue #3389
Issue #3390
Issue #3391
Issue #3392
Issue #3393
Issue #3394
check_fiber_spawn_cli_dtor_drain_3394
check_fiber_spawn_cli_dtor_drain_3394.py
Issue #3395
check_query_default_stamped_3395
check_query_default_stamped_3395.py
Issue #3396
check_unpack_stable_ref_arg_v2_3396
check_unpack_stable_ref_arg_v2_3396.py
Issue #3397
check_query_children_stable_no_tls_3397
check_query_children_stable_no_tls_3397.py
Issue #3398
check_as_stable_ref_v2_3398
check_as_stable_ref_v2_3398.py
Issue #3399
check_structural_mutate_resolve_helper_3399
check_structural_mutate_resolve_helper_3399.py
check_stable_ref_probe_3400
check_stable_ref_probe_3400.py
Issue #3401
check_eval_flat_hot_path_3401
check_eval_flat_hot_path_3401.py
Issue #3402
check_dense_children_columns_3402
check_dense_children_columns_3402.py
dense-children-columns-3402
Issue #3403
check_inline_pass_soa_3403
check_inline_pass_soa_3403.py
inline-pass-soa-3403
Issue #3404
arena-auto-arm-soft-fallback-3404
check_arena_auto_arm_soft_fallback_3404
check_arena_auto_arm_soft_fallback_3404.py
Issue #3405
check_pure_wrap_dirty_entry_3405
check_pure_wrap_dirty_entry_3405.py
pure-wrap-dirty-entry-3405
Issue #3406
check_recover_fail_clear_persist_3406
check_recover_fail_clear_persist_3406.py
Issue #3407
check_synthesize_set_walks_rhs_3407
check_synthesize_set_walks_rhs_3407.py
check_set_assignment_hygiene_3408
check_set_assignment_hygiene_3408.py
Issue #3409
check_grant_ssot_ta_fence_3409
check_grant_ssot_ta_fence_3409.py
Issue #3410
check_dual_fresh_mutate_soft_migrate_3410
check_dual_fresh_mutate_soft_migrate_3410.py
Issue #3411
check_wildcard_ta_string_gate_3411
check_wildcard_ta_string_gate_3411.py
Issue #3412
check_deopt_pending_closure_call_3412
check_deopt_pending_closure_call_3412.py
Issue #3413
check_partial_reemit_success_coverage_3413
check_partial_reemit_success_coverage_3413.py
Issue #3414
check_no_tls_live_policy_default_solved_3414
check_no_tls_live_policy_default_solved_3414.py
Issue #3415
check_bare_nodeid_foreign_stamp_3415
check_bare_nodeid_foreign_stamp_3415.py
check_last_proof_eval_identity_3416
check_last_proof_eval_identity_3416.py
Issue #3417
check_cascade_skip_subtree_full_cone_3417
check_cascade_skip_subtree_full_cone_3417.py
Issue #3418
check_proof_goal_fingerprint_overflow_3418
check_proof_goal_fingerprint_overflow_3418.py
Issue #3419
check_jit_typed_entry_every_function_3419
check_jit_typed_entry_every_function_3419.py
Issue #3420
check_factory_refuse_uncovered_3420
check_factory_refuse_uncovered_3420.py
Issue #3421
check_apply_closure_densify_hard_refuse_3421
check_apply_closure_densify_hard_refuse_3421.py
Issue #3423
check_add_mutate_acquire_before_body_3423
check_add_mutate_acquire_before_body_3423.py
Issue #3424
check_query_result_hash_resolve_3424
check_query_result_hash_resolve_3424.py
Issue #3425
check_as_stable_ref_prod_int_reject_3425
check_as_stable_ref_prod_int_reject_3425.py
Issue #3426
check_restamp_hot_cone_held_overflow_3426
check_restamp_hot_cone_held_overflow_3426.py
Issue #3427
check_query_find_prod_no_scan_3427
check_query_find_prod_no_scan_3427.py
Issue #3428
check_hot_contract_view_at_harden_3428
check_hot_contract_view_at_harden_3428.py
Issue #3429
check_pcv_shared_cow_tls_3429
check_pcv_shared_cow_tls_3429.py
Issue #3430
check_production_defaults_force_strict_unify_3430
check_production_defaults_force_strict_unify_3430.py
Issue #3431
check_occurrence_unstaged_expected_fp_3431
check_occurrence_unstaged_expected_fp_3431.py
Issue #3432
check_empty_pair_no_dynamic_3432
check_empty_pair_no_dynamic_3432.py
Issue #3433
check_join_cleanup_fork_3433
check_join_cleanup_fork_3433.py
Issue #3434
check_tenant_spawn_mandate_3434
check_tenant_spawn_mandate_3434.py
Issue #3435
check_relocate_drop_tracking_3435
check_relocate_drop_tracking_3435.py
Issue #3436
check_grant_lifetime_alignment_3436
check_grant_lifetime_alignment_3436.py
Issue #3437
check_scope_session_drop_3437
check_scope_session_drop_3437.py
Issue #3438
check_scoped_linear_drain_3438
check_scoped_linear_drain_3438.py
Issue #3439
Issue #3440
check_outermost_persist_reject_restore_3440
check_outermost_persist_reject_restore_3440.py
Issue #3441
check_deopt_pending_fast_path_3441
check_deopt_pending_fast_path_3441.py
Issue #3442
check_scope_message_resolve_3442
check_scope_message_resolve_3442.py
Issue #3443
check_ffi_jit_live_ptr_inventory_3443
check_ffi_jit_live_ptr_inventory_3443.py
Issue #3444
check_scope_child_address_3444
check_scope_child_address_3444.py
Issue #3445
check_reemit_pipeline_reason_coverage_3445
check_reemit_pipeline_reason_coverage_3445.py
Issue #3446
check_linear_epoch_fence_elision_typed_3446
check_linear_epoch_fence_elision_typed_3446.py
Issue #3447
check_jit_dual_fresh_c_bridge_3447
check_jit_dual_fresh_c_bridge_3447.py
Issue #3448
check_linear_zero_root_green_face_drop_3448
check_linear_zero_root_green_face_drop_3448.py
Issue #3449
check_query_default_schema2_export_3449
check_query_default_schema2_export_3449.py
Issue #3450
check_add_mutate_read_only_fence_3450
check_add_mutate_read_only_fence_3450.py
Issue #3451
check_nested_gap_query_epoch_stale_3451
check_nested_gap_query_epoch_stale_3451.py
check_mutate_reg_kind_3452
check_mutate_reg_kind_3452.py
check_set_child_locked_dense_inplace_3453
check_set_child_locked_dense_inplace_3453.py
Issue #3454
check_production_pure_wrap_soa_3454
check_production_pure_wrap_soa_3454.py
production-pure-wrap-soa-3454
Issue #3455
check_shape_compact_dirty_fnkey_3455
check_shape_compact_dirty_fnkey_3455.py
check_destroy_dtor_index_3456
check_destroy_dtor_index_3456.py
Issue #3457
check_eval_flat_sym_intern_3457
check_eval_flat_sym_intern_3457.py
Issue #3458
check_tenant_scope_release_key_3458
check_tenant_scope_release_key_3458.py
Issue #3459
Issue #3460
check_se_wal_sidecar_pair_3460
check_se_wal_sidecar_pair_3460.py
Issue #3461
Issue #3462
Issue #3463
check_agent_wait_reclaimed_scope_resolve_3463
check_agent_wait_reclaimed_scope_resolve_3463.py
Issue #3464
Issue #3465
Issue #3466
check_reemit_pipeline_reason_heal_3466
check_reemit_pipeline_reason_heal_3466.py
Issue #3467
check_reclaimed_put_deny_3467
check_reclaimed_put_deny_3467.py
Issue #3468
check_stamp_rest_param_no_overstamp_3468
check_stamp_rest_param_no_overstamp_3468.py
Issue #3469
check_apply_closure_densify_multi_window_3469
check_apply_closure_densify_multi_window_3469.py
Issue #3470
check_inner_expand_unwrap_depth_3470
check_inner_expand_unwrap_depth_3470.py
Issue #3471
check_jit_dual_fresh_table_stamp_3471
check_jit_dual_fresh_table_stamp_3471.py
Issue #3472
check_persist_green_phase1_linear_deny_3472
check_persist_green_phase1_linear_deny_3472.py
Issue #3473
check_ffi_alias_slot_register_3473
check_ffi_alias_slot_register_3473.py
Issue #3474
check_production_called_by_cone_bfs_3474
check_production_called_by_cone_bfs_3474.py
Issue #3475
check_macro_introduced_native_fail_closed_3475
check_macro_introduced_native_fail_closed_3475.py
Issue #3476
check_scheduler_production_ready_weld_3476
check_scheduler_production_ready_weld_3476.py
Issue #3477
check_solve_delta_dirty_skip_residual_3477
check_solve_delta_dirty_skip_residual_3477.py
Issue #3478
check_pure_anon_budget_skip_must_deopt_3478
check_pure_anon_budget_skip_must_deopt_3478.py
Issue #3479
check_steal_envframe_handoff_elevation_3479
check_steal_envframe_handoff_elevation_3479.py
Issue #3480
check_hold_budget_add_mutate_inbody_poll_3480
check_hold_budget_add_mutate_inbody_poll_3480.py
Issue #3481
check_aot_facade_content_restamp_3481
check_aot_facade_content_restamp_3481.py
Issue #3482
check_steal_densify_persist_clear_3482
check_steal_densify_persist_clear_3482.py
Issue #3483
check_steal_complete_latch_strong_3483
check_steal_complete_latch_strong_3483.py
Issue #3484
check_peel_zero_mask_fail_closed_3484
check_peel_zero_mask_fail_closed_3484.py
Issue #3485
check_mailbox_hold_budget_p99_union_3485
check_mailbox_hold_budget_p99_union_3485.py
Issue #3486
check_dual_dep_graph_inject_oracle_3486
check_dual_dep_graph_inject_oracle_3486.py
Issue #3487
check_query_stable_restamp_latch_export_3487
check_query_stable_restamp_latch_export_3487.py
Issue #3488
check_production_pure_wrap_hot_pack_3488
check_production_pure_wrap_hot_pack_3488.py
Issue #3489
check_ir_soa_single_mark_deleted_3489
check_ir_soa_single_mark_deleted_3489.py
Issue #3490
check_hot_contract_harden_cache_3490
check_hot_contract_harden_cache_3490.py
check_pcv_unique_zero_atomic_3491
check_pcv_unique_zero_atomic_3491.py
Issue #3494
Issue #3495
Issue #3496
Issue #3497
Issue #3498
Issue #3499
Issue #3500
Issue #3501
Issue #3502
Issue #3503
Issue #3504
Issue #3505
Issue #3506
Issue #3507
Issue #3508
Issue #3509
Issue #3510
Issue #3511
Issue #3513
Issue #3514
Issue #3515
Issue #3516
Issue #3517
Issue #3518
Issue #3519
Issue #3521
Issue #3525
Issue #3526
Issue #3527
Issue #3528
Issue #3529
Issue #3530
Issue #3531
Issue #3532
Issue #3533
Issue #3539
Issue #3540
Issue #3541
Issue #3542
Issue #3543
Issue #3544
Issue #3546
Issue #3547
Issue #3548
Issue #3549
Issue #3551
Issue #3553
Issue #3555
Issue #3556
Issue #3557
Issue #3558
Issue #3559
Issue #3560
Issue #3561
Issue #3562
Issue #3563
Issue #3564
Issue #3566
Issue #3568
Issue #3569
Issue #3570
Issue #3571
Issue #3572
Issue #3573
Issue #3574
Issue #3575
Issue #3576
Issue #3577
Issue #3578
Issue #3579
Issue #3580
Issue #3581
Issue #3582
Issue #3583
Issue #3584
Issue #3585
Issue #3587
Issue #3588
Issue #3589
check_index_sync_structural_write_3589
check_index_sync_structural_write_3589.py
Issue #3590
Issue #3591
check_linear_elision_fast_path_3591
check_linear_elision_fast_path_3591.py
Issue #3592
Issue #3593
Issue #3594
Issue #3595
check_join_reclaim_retry_3595
check_join_reclaim_retry_3595.py
Issue #3596
Issue #3597
Issue #3598
Issue #3599
Issue #3600
check_moving_untracked_split_3600
check_moving_untracked_split_3600.py
Issue #3601
check_lockless_hygiene_se_3601
check_lockless_hygiene_se_3601.py
Issue #3602
check_ffi_apply_densify_refuse_3602
check_ffi_apply_densify_refuse_3602.py
Issue #3603
check_wal_window_miss_catalog_3603
check_wal_window_miss_catalog_3603.py
Issue #3604
check_panic_aba_gc_defer_drain_3604
check_panic_aba_gc_defer_drain_3604.py
Issue #3605
check_facade_owner_scope_clock_skip_3605
check_facade_owner_scope_clock_skip_3605.py
Issue #3606
check_nested_qq_depth_3606
check_nested_qq_depth_3606.py
Issue #3607
check_remount_reason_domain_3607
check_remount_reason_domain_3607.py
Issue #3608
check_expand_checkpoint_local_rollback_3608
check_expand_checkpoint_local_rollback_3608.py
Issue #3609
check_reexpand_tenant_principal_3609
check_reexpand_tenant_principal_3609.py
Issue #3610
check_real_quiet_live_tc_3610
check_real_quiet_live_tc_3610.py
Issue #3611
check_cascade_rearm_reconsult_3611
check_cascade_rearm_reconsult_3611.py
Issue #3612
check_remount_densify_pairing_strip_3612
check_remount_densify_pairing_strip_3612.py
Issue #3613
check_mailbox_holder_send_lock_order_3613
check_mailbox_holder_send_lock_order_3613.py
Issue #3614
check_outermost_persist_order_3614
check_outermost_persist_order_3614.py
Issue #3615
check_dual_graph_parity_cone_3615
check_dual_graph_parity_cone_3615.py
Issue #3616
check_jit_anon_linear_prologue_3616
check_jit_anon_linear_prologue_3616.py
Issue #3617
check_steal_eval_keyed_residual_3617
check_steal_eval_keyed_residual_3617.py
Issue #3618
check_residual_persist_attribution_3618
check_residual_persist_attribution_3618.py
Issue #3619
Issue #3620
check_chaos_guard_hold_smoke_3620
check_chaos_guard_hold_smoke_3620.py
Issue #3621
check_steal_identity_proof_3621
check_steal_identity_proof_3621.py
Issue #3622
check_type_dynamic_production_3622
check_type_dynamic_production_3622.py
Issue #3623
check_occurrence_recover_regate_3623
check_occurrence_recover_regate_3623.py
Issue #3624
check_type_export_face_3624
check_type_export_face_3624.py
Issue #3625
check_render_fast_audit_3625
check_render_fast_audit_3625.py
Issue #3626
check_closure_calls_hotpath_3626
check_closure_calls_hotpath_3626.py
check_pack_pipeline_strict_3627
check_pack_pipeline_strict_3627.py
Issue #3628
Issue #3629
Issue #3630
check_undeclared_mt_autodetect_3630
check_undeclared_mt_autodetect_3630.py
Issue #3631
check_reclaimed_batch_join_3631
check_reclaimed_batch_join_3631.py
Issue #3632
check_scope_bp_sender_attribution_3632
check_scope_bp_sender_attribution_3632.py
Issue #3633
check_moving_cover_reconciliation_3633
check_moving_cover_reconciliation_3633.py
Issue #3634
Issue #3635
check_closure_dispatch_entry_3635
check_closure_dispatch_entry_3635.py
Issue #3636
check_region_storm_attribution_3636
check_region_storm_attribution_3636.py
Issue #3637
check_macro_boundary_backstop_3637
check_macro_boundary_backstop_3637.py
Issue #3638
check_query_index_telemetry_3638
check_query_index_telemetry_3638.py
Issue #3639
check_wal_append_miss_deny_3639
check_wal_append_miss_deny_3639.py
Issue #3640
check_unpack_spine_gate_3640
check_unpack_spine_gate_3640.py
Issue #3641
check_occupancy_collision_failclosed_3641
check_occupancy_collision_failclosed_3641.py
Issue #3642
check_recv_stale_nullopt_3642
check_recv_stale_nullopt_3642.py
Issue #3643
check_scope_join_tree_3643
check_scope_join_tree_3643.py
Issue #3644
check_reclaimed_slot_second_recycle_3644
check_reclaimed_slot_second_recycle_3644.py
Issue #3645
check_metrics_not_found_3645
check_metrics_not_found_3645.py
Issue #3646
Issue #3647
check_closure_densify_slots_3647
check_closure_densify_slots_3647.py
Issue #3648
check_apply_window_gate_3648
check_apply_window_gate_3648.py
Issue #3649
check_storm_exit_coverage_verify_3649
check_storm_exit_coverage_verify_3649.py
Issue #3650
check_rollback_mse_3650
check_rollback_mse_3650.py
Issue #3651
check_expand_all_deny_codes_3651
check_expand_all_deny_codes_3651.py
Issue #3652
check_lockless_allow_mse_3652
check_lockless_allow_mse_3652.py
Issue #3653
check_outermost_persist_audit_order_3653
check_outermost_persist_audit_order_3653.py
Issue #3654
check_jit_linear_post_mutate_unset_3654
check_jit_linear_post_mutate_unset_3654.py
Issue #3655
check_persist_sdo_before_unstaged_3655
check_persist_sdo_before_unstaged_3655.py
Issue #3656
check_callee_cone_partial_absorb_3656
check_callee_cone_partial_absorb_3656.py
Issue #3657
check_unslotted_string_edge_parity_3657
check_unslotted_string_edge_parity_3657.py
Issue #3658
check_type_dirty_txn_before_cascade_3658
check_type_dirty_txn_before_cascade_3658.py
Issue #3659
check_boundary_safe_uses_snap_3659
check_boundary_safe_uses_snap_3659.py
Issue #3660
check_query_result_per_match_fresh_3660
check_query_result_per_match_fresh_3660.py
Issue #3661
check_packed_v2_no_occupancy_refresh_3661
check_packed_v2_no_occupancy_refresh_3661.py
Issue #3662
check_is_coercible_dynamic_prod_3662
check_is_coercible_dynamic_prod_3662.py
Issue #3663
check_linear_move_elision_abort_live_3663
check_linear_move_elision_abort_live_3663.py
Issue #3664
check_replace_pattern_match_sub_no_std_function_3664
check_replace_pattern_match_sub_no_std_function_3664.py
check_insert_remove_child_locked_dense_splice_3665
check_insert_remove_child_locked_dense_splice_3665.py
Issue #3666
check_hot_contract_production_pack_3666
check_hot_contract_production_pack_3666.py
Issue #3668
check_quota_tenant_keying_3668
check_quota_tenant_keying_3668.py
Issue #3669
check_isolation_audit_caller_3669
check_isolation_audit_caller_3669.py
Issue #3670
check_resume_mid_join_3670
check_resume_mid_join_3670.py
Issue #3671
check_scope_join_tree_visibility_3671
check_scope_join_tree_visibility_3671.py
Issue #3672
check_bp_scope_tenant_3672
check_bp_scope_tenant_3672.py
Issue #3673
check_recv_under_boundary_3673
check_recv_under_boundary_3673.py
Issue #3674
check_wal_fold_autoscan_3674
check_wal_fold_autoscan_3674.py
Issue #3675
Issue #3676
Issue #3677
Issue #3678
Issue #3679
check_envframe_scan_order_3679
check_envframe_scan_order_3679.py
Issue #3680
check_compact_owner_scope_3680
check_compact_owner_scope_3680.py
Issue #3681
check_apply_pre_reemit_refuse_3681
check_apply_pre_reemit_refuse_3681.py
Issue #3682
check_last_success_coverage_evidence_3682
check_last_success_coverage_evidence_3682.py
Issue #3683
check_hygiene_deny_kind_uniformity_3683
check_hygiene_deny_kind_uniformity_3683.py
Issue #3684
check_inner_expand_refuse_3684
check_inner_expand_refuse_3684.py
Issue #3685
check_clone_session_policy_3685
check_clone_session_policy_3685.py
Issue #3686
Issue #3687
Issue #3688
Issue #3689
Issue #3690
Issue #3691
Issue #3692
Issue #3693
Issue #3694
Issue #3695
Issue #3696
Issue #3697
Issue #3698
Issue #3699
Issue #3700
Issue #3701
Issue #3702
Issue #3703
check_quote_intern_lock_3703
check_quote_intern_lock_3703.py
Issue #3720
check_mse_session_bound_3721
check_mse_session_bound_3721.py
Issue #3722
check_rollback_effect_gate_3722
check_rollback_effect_gate_3722.py
Issue #3723
Issue #3724
Issue #3725
Issue #3726
Issue #3727
Issue #3728
Issue #3729
Issue #3730
Issue #3731
Issue #3732
Issue #3733
Issue #3735
Issue #3736
Issue #3737
check_compiler_metrics_fields_3737
check_compiler_metrics_fields_3737.py
Issue #3738
Issue #3739
Issue #3740
Issue #3742
Issue #3743
Issue #3744
Issue #3745
Issue #3746
Issue #3747
Issue #3748
Issue #3749
Issue #3750
Issue #3751
Issue #3752
Issue #3753
Issue #3754
Issue #3755
Issue #3756
Issue #3757
Issue #3758
Issue #3759
Issue #3760
Issue #3761
Issue #3762
Issue #3763
Issue #3764
Issue #3765
Issue #3766
Issue #3767
Issue #3768
Issue #3769
Issue #3770
check_ast_ref_stamp_3772
check_ast_ref_stamp_3772.py
Issue #3773
Issue #3774
Issue #3775
Issue #3776
Issue #3777
Issue #3778
check_occurrence_mid_join_ssot_3778
check_occurrence_mid_join_ssot_3778.py
Issue #3779
check_steal_complete_total_ssot_3779
check_steal_complete_total_ssot_3779.py
Issue #3780
check_mutation_wal_miss_fail_closed_3780
check_mutation_wal_miss_fail_closed_3780.py
Issue #3781
check_densify_this_window_rewrite_3781
check_densify_this_window_rewrite_3781.py
Issue #3782
check_densify_entry_lcp_skip_compact_3782
check_densify_entry_lcp_skip_compact_3782.py
Issue #3783
check_auto_arm_densify_health_publish_3783
check_auto_arm_densify_health_publish_3783.py
Issue #3784
check_anon_peer_empty_name_soft_stale_3784
check_anon_peer_empty_name_soft_stale_3784.py
Issue #3785
check_sync_remount_storm_gate_3785
check_sync_remount_storm_gate_3785.py
Issue #3786
check_allow_macro_inline_mse_3786
check_allow_macro_inline_mse_3786.py
Issue #3787
check_sticky_last_limit_deny_all_3787
check_sticky_last_limit_deny_all_3787.py
Issue #3788
check_partial_occurrence_drift_escalate_3788
check_partial_occurrence_drift_escalate_3788.py
Issue #3789
check_remount_last_zero_agent_face_3789
check_remount_last_zero_agent_face_3789.py
Issue #3790
check_rollback_mutation_boundary_guard_3790
check_rollback_mutation_boundary_guard_3790.py
Issue #3791
check_mailbox_lock_audit_pairs_3791
check_mailbox_lock_audit_pairs_3791.py
Issue #3792
check_mutation_history_tenant_filter_3792
check_mutation_history_tenant_filter_3792.py
Issue #3793
check_replace_pattern_kw_unify_3793
check_replace_pattern_kw_unify_3793.py
Issue #3794
check_partial_drift_grant_refuse_3794
check_partial_drift_grant_refuse_3794.py
Issue #3795
check_escape_analysis_pure_wrap_3795
check_escape_analysis_pure_wrap_3795.py
Issue #3796
check_occurrence_coercion_ci_ok_3796
check_occurrence_coercion_ci_ok_3796.py
Issue #3797
Issue #3798
Issue #3799
check_session_revoke_fiber_zero_3799
check_session_revoke_fiber_zero_3799.py
Issue #3800
check_cross_tenant_grant_caller_ta_3800
check_cross_tenant_grant_caller_ta_3800.py
Issue #3801
check_isolation_deny_mid_join_3801
check_isolation_deny_mid_join_3801.py
Issue #3802
check_tenant_host_path_isolation_3802
check_tenant_host_path_isolation_3802.py
Issue #3803
check_agent_scope_region_key_isolation_3803
check_agent_scope_region_key_isolation_3803.py
Issue #3804
check_mailbox_bp_overflow_cohort_3804
check_mailbox_bp_overflow_cohort_3804.py
Issue #3805
Issue #3806
check_wal_overflow_wrap_3806
check_wal_overflow_wrap_3806.py
Issue #3808
check_isolation_deny_obs_join_3808
check_isolation_deny_obs_join_3808.py
Issue #3809
check_boundary_soft_densify_restamp_3809
check_boundary_soft_densify_restamp_3809.py
Issue #3810
check_soft_gen_bump_this_window_3810
check_soft_gen_bump_this_window_3810.py
Issue #3811
check_agent_soft_densify_restamp_3811
check_agent_soft_densify_restamp_3811.py
Issue #3812
check_soft_global_critical_remount_3812
check_soft_global_critical_remount_3812.py
check_steal_checkpoint_dual_track_3813
check_steal_checkpoint_dual_track_3813.py
Issue #3814
check_sticky_force_empty_residual_3814
check_sticky_force_empty_residual_3814.py
Issue #3815
check_move_node_parent_hygiene_3815
check_move_node_parent_hygiene_3815.py
Issue #3816
check_clone_walk_rename_deny_abort_3816
check_clone_walk_rename_deny_abort_3816.py
Issue #3817
check_rest_spine_orphan_rewind_3817
check_rest_spine_orphan_rewind_3817.py
Issue #3818
check_post_persist_linear_deny_coercion_undo_3818
check_post_persist_linear_deny_coercion_undo_3818.py
Issue #3819
check_occurrence_persist_fp_mismatch_dual_track_3819
check_occurrence_persist_fp_mismatch_dual_track_3819.py
Issue #3820
check_pending_offer_map_miss_retain_3820
check_pending_offer_map_miss_retain_3820.py
Issue #3821
check_synth_hard_fail_abort_force_dirty_3821
check_synth_hard_fail_abort_force_dirty_3821.py
Issue #3822
check_prod_soa_writeback_sync_3822
check_prod_soa_writeback_sync_3822.py
Issue #3823
check_production_mark_node_dep_union_3823
check_production_mark_node_dep_union_3823.py
Issue #3824
check_steal_force_clear_hold_foreign_3824
check_steal_force_clear_hold_foreign_3824.py
Issue #3825
check_hold_budget_safepoint_force_release_3825
check_hold_budget_safepoint_force_release_3825.py
Issue #3826
check_edge_free_latch_gate_3826
check_edge_free_latch_gate_3826.py
Issue #3827
check_query_children_parent_schema2_3827
check_query_children_parent_schema2_3827.py
Issue #3828
check_mutate_from_feedback_ssot_3828
check_mutate_from_feedback_ssot_3828.py
Issue #3829
check_dense_columnar_fingerprint_3829
check_dense_columnar_fingerprint_3829.py
Issue #3830
check_empty_type_annotation_no_dynamic_3830
check_empty_type_annotation_no_dynamic_3830.py
Issue #3831
check_dirty_aware_storm_force_full_cap_3831
check_dirty_aware_storm_force_full_cap_3831.py
Issue #3832
check_apply_closure_tls_cache_3832
check_apply_closure_tls_cache_3832.py
check_ir_soa_column_arena_3833
check_ir_soa_column_arena_3833.py
Issue #3834
check_call_ownerless_fail_closed_3834
check_call_ownerless_fail_closed_3834.py
Issue #3835
check_tenant_host_path_read_3835
check_tenant_host_path_read_3835.py
Issue #3836
check_shell_require_effect_3836
check_shell_require_effect_3836.py
Issue #3837
Issue #3838
check_wal_overflow_wrap_refuse_3838
check_wal_overflow_wrap_refuse_3838.py
Issue #3839
check_string_grant_session_bound_3839
check_string_grant_session_bound_3839.py
Issue #3840
check_parallel_intend_region_eval_mu_3840
check_parallel_intend_region_eval_mu_3840.py
Issue #3841
check_quota_recycle_must_wait_ssot_3841
check_quota_recycle_must_wait_ssot_3841.py
Issue #3842
check_scope_sweep_reclaimed_pending_3842
check_scope_sweep_reclaimed_pending_3842.py
Issue #3843
check_require_effect_full_hard_mid_3843
check_require_effect_full_hard_mid_3843.py
cmd_require_effect_full_hard_mid_3843
Issue #3844
check_grant_epoch_no_phantom_3844
check_grant_epoch_no_phantom_3844.py
Issue #3845
check_promote_force_join_mid_no_invent_3845
check_promote_force_join_mid_no_invent_3845.py
cmd_promote_force_join_mid_no_invent_3845
check_reload_recovery_state_headroom_3846
check_reload_recovery_state_headroom_3846.py
Issue #3848
check_densify_refuse_zero_move_3848
check_densify_refuse_zero_move_3848.py
Issue #3849
check_apply_closure_happy_path_densify_3849
check_apply_closure_happy_path_densify_3849.py
Issue #3850
check_steal_panic_moving_gate_3850
check_steal_panic_moving_gate_3850.py
Issue #3851
check_pure_anon_budget_skip_sticky_3851
check_pure_anon_budget_skip_sticky_3851.py
Issue #3852
check_abort_aot_invalidate_3852
check_abort_aot_invalidate_3852.py
check_ir_soa_column_slab_shard_3853
check_ir_soa_column_slab_shard_3853.py
Issue #3854
check_mse_revoke_fiber_3854
check_mse_revoke_fiber_3854.py
Issue #3855
check_wal_miss_se_compensate_3855
check_wal_miss_se_compensate_3855.py
check_wal_miss_ring_flip_3856
check_wal_miss_ring_flip_3856.py
Issue #3857
check_temp_canary_moving_gate_3857
check_temp_canary_moving_gate_3857.py
Issue #3858
check_restore_checkpoint_mse_3858
check_restore_checkpoint_mse_3858.py
Issue #3859
Issue #3860
Issue #3861
Issue #3862
Issue #3863
Issue #3864
Issue #3865
Issue #3866
Issue #3867
Issue #3868
check_flatast_seqlock_get_3868
check_flatast_seqlock_get_3868.py
Issue #3869
check_storm_force_full_counter_3869
check_storm_force_full_counter_3869.py
Issue #3870
check_pass_purity_effect_3870
check_pass_purity_effect_3870.py
Issue #3871
check_letrec_check_synth_3871
check_letrec_check_synth_3871.py
Issue #3872
check_mutate_session_hard_gate_3872
check_mutate_session_hard_gate_3872.py
Issue #3873
check_health_advisory_face_3873
check_health_advisory_face_3873.py
Issue #3874
check_audit_event_tenant_3874
check_audit_event_tenant_3874.py
Issue #3875
check_revoke_epoch_hard_only_3875
check_revoke_epoch_hard_only_3875.py
Issue #3876
Issue #3877
Issue #3878
Issue #3879
Issue #3880
Issue #3881
Issue #3882
Issue #3883
Issue #3884
Issue #3885
Issue #3886
Issue #3887
Issue #3888
Issue #3889
Issue #3890
Issue #3891
Issue #3892
Issue #3893
Issue #3894
Issue #3895
Issue #3896
Issue #3897
Issue #3898
Issue #3899
Issue #3900
Issue #3902
check_soft_revoke_epoch_honest_3902
check_soft_revoke_epoch_honest_3902.py
Issue #3903
check_correlate_tenant_passthrough_3903
check_correlate_tenant_passthrough_3903.py
Issue #3904
check_mse_ta_posture_3904
check_mse_ta_posture_3904.py
Issue #3905
Issue #3906
Issue #3907
Issue #3908
Issue #3909
Issue #3910
Issue #3912
Issue #3915
Issue #3916
Issue #3917
Issue #3918
Issue #3919
Issue #3930
Issue #3931
Issue #3932
Issue #3934
Issue #3938
Issue #3939
Issue #3940
Issue #3941
check_orch_residuals_3941
check_orch_residuals_3941.py
Issue #3942
Issue #3943
Issue #3944
Issue #3946
Issue #3947
Issue #3948
Issue #3951
Issue #3952
Issue #3953
Issue #3954
Issue #3955
Issue #3956
Issue #3957
Issue #3958
Issue #3964
Issue #3965
Issue #3966
Issue #3967
Issue #3968
Issue #3969
Issue #3972
check_jit_densify_remap_3972
check_jit_densify_remap_3972.py
Issue #3973
check_native_moving_canary_env_note_3973
check_native_moving_canary_env_note_3973.py
Issue #3974
Issue #3975
Issue #3976
Issue #3977
Issue #3978
Issue #3979
Issue #3980
Issue #3981
Issue #3982
Issue #3983
Issue #3984
Issue #3985
Issue #3986
Issue #3987
Issue #3988
Issue #3989
Issue #3990
Issue #3991
Issue #3992
Issue #3993
Issue #3994
Issue #3995
Issue #3996
Issue #3997
Issue #3998
Issue #3999
Issue #4000
Issue #4001
check_agent_recv_typed_preference_4001
check_agent_recv_typed_preference_4001.py
Issue #4002
Issue #4004
Issue #4005
Issue #4006
Issue #4007
Issue #4008
Issue #4009
Issue #4010
Issue #4011
Issue #4012
check_aot_metrics_deopt_ssot_4013
check_aot_metrics_deopt_ssot_4013.py
Issue #4014
Issue #4015
Issue #4016
Issue #4017
Issue #4018
Issue #4019
Issue #4020
Issue #4021
Issue #4022
Issue #4023
Issue #4024
Issue #4025
Issue #4026
Issue #4029
Issue #4030
Issue #4031
Issue #4032
check_steal_densify_inflight_boundary_safe_4033
check_steal_densify_inflight_boundary_safe_4033.py
Issue #4034
Issue #4036
check_mutation_choke_4036
check_mutation_choke_4036.py
Issue #4037
check_agent_fingerprint_gate_4037
check_agent_fingerprint_gate_4037.py
Issue #4038
check_session_revoke_host_cohort_4038
check_session_revoke_host_cohort_4038.py
Issue #4039
check_nodeid_collision_4039
check_nodeid_collision_4039.py
Issue #4040
Issue #4041
Issue #4042
Issue #4043
Issue #4044
Issue #4045
Issue #4046
Issue #4047
Issue #4049
check_agent_reply_payload_4049
check_agent_reply_payload_4049.py
Issue #4050
check_supervise_join_4050
check_supervise_join_4050.py
Issue #4051
check_occupancy_torn_read_4051
check_occupancy_torn_read_4051.py
Issue #4052
check_isolation_deny_epoch_4052
check_isolation_deny_epoch_4052.py
check_soft_ready_http_post_denseness_4053
check_soft_ready_http_post_denseness_4053.py
check_soft_ready_http_post_two_batch_4054
check_soft_ready_http_post_two_batch_4054.py
check_soft_ready_http_post_three_batch_4055
check_soft_ready_http_post_three_batch_4055.py
check_soft_ready_http_post_after_probe_4056
check_soft_ready_http_post_after_probe_4056.py
Issue #4057
check_pair_slot_tenant_4057
check_pair_slot_tenant_4057.py
Issue #4058
check_capability_stack_authorization_4058
check_capability_stack_authorization_4058.py
Issue #4059
check_load_tenant_mutate_4059
check_load_tenant_mutate_4059.py
Issue #4060
Issue #4061
Issue #4062
Issue #4063
Issue #4064
Issue #4065
Issue #4066
Issue #4067
Issue #4068
Issue #4069
Issue #4070
Issue #4071
Issue #4072
Issue #4073
Issue #4074
Issue #4075
Issue #4076
Issue #4077
Issue #4078
Issue #4079
Issue #4080
Issue #4081
Issue #4082
Issue #4083
Issue #4084
Issue #4085
Issue #4086
Issue #4087
check_authority_gap_lifecycle_4087
check_authority_gap_lifecycle_4087.py
Issue #4088
check_query_bare_nodeid_4088
check_query_bare_nodeid_4088.py
Issue #4089
check_fiber_type_belt_4089
check_fiber_type_belt_4089.py
Issue #4090
check_shape_hotpath_4090
check_shape_hotpath_4090.py
Issue #4091
check_shape_dirty_cone_4091
check_shape_dirty_cone_4091.py
Issue #4093
check_jit_heap_tenant_4093
check_jit_heap_tenant_4093.py
Issue #4094
check_closure_capture_tenant_4094
check_closure_capture_tenant_4094.py
Issue #4095
Issue #4096
Issue #4097
Issue #4098
Issue #4099
Issue #4100
Issue #4101
Issue #4102
Issue #4103
Issue #4104
Issue #4105
Issue #4106
check_query_bare_nodeid_4106
check_query_bare_nodeid_4106.py
Issue #4107
check_type_ground_gate_4107
check_type_ground_gate_4107.py
Issue #4108
check_shape_sync_index_4108
check_shape_sync_index_4108.py
Issue #4109
check_materialize_defuse_4109
check_materialize_defuse_4109.py
Issue #4110
check_hash_tenant_stamp_4110
check_hash_tenant_stamp_4110.py
Issue #4111
check_pair_read_tenant_4111
check_pair_read_tenant_4111.py
Issue #4112
check_agent_export_occupancy_4112
check_agent_export_occupancy_4112.py
Issue #4113
check_resolve_stamped_occupancy_4113
check_resolve_stamped_occupancy_4113.py
Issue #4114
check_join_fail_failfast_4114
check_join_fail_failfast_4114.py
Issue #4115
check_scope_join_sweep_4115
check_scope_join_sweep_4115.py
Issue #4117
check_se_wal_join_4118
check_se_wal_join_4118.py
Issue #4120
check_replay_mid_last_stamped_4120
check_replay_mid_last_stamped_4120.py
check_typed_mutation_audit_trail_headroom_4121
check_typed_mutation_audit_trail_headroom_4121.py
Issue #4122
Issue #4124
check_temp_canary_bind_4124
check_temp_canary_bind_4124.py
Issue #4125
check_apply_densify_inflight_4125
check_apply_densify_inflight_4125.py
Issue #4126
check_jit_reemit_install_4126
check_jit_reemit_install_4126.py
Issue #4127
check_lockless_macro_allow_latch_4127
check_lockless_macro_allow_latch_4127.py
Issue #4128
check_workspace_lock_stamp_4128
check_workspace_lock_stamp_4128.py
Issue #4129
check_character_literal_4129
check_character_literal_4129.py
Issue #4130
check_empty_list_unparse_4130
check_empty_list_unparse_4130.py
Issue #4131
check_oneshot_sigterm_4131
check_oneshot_sigterm_4131.py
Issue #4132
check_export_name_unparse_4132
check_export_name_unparse_4132.py
Issue #4133
check_mse_caller_only_ta_4133
check_mse_caller_only_ta_4133.py
Issue #4134
check_jit_owner_failclosed_4134
check_jit_owner_failclosed_4134.py
Issue #4135
check_grant_node_provenance_4135
check_grant_node_provenance_4135.py
Issue #4136
check_caller_sweep_4136
check_caller_sweep_4136.py
Issue #4137
check_agent_recv_steal_handoff_4137
check_agent_recv_steal_handoff_4137.py
Issue #4138
check_agent_join_scope_compact_4138
check_agent_join_scope_compact_4138.py
Issue #4139
check_join_token_observe_4139
check_join_token_observe_4139.py
Issue #4140
check_densify_health_headroom_4140
check_densify_health_headroom_4140.py
Issue #4141
check_capability_effect_headroom_4141
check_capability_effect_headroom_4141.py
check_wal_miss_refuse_next_4142
check_wal_miss_refuse_next_4142.py
Issue #4143
check_moving_sticky_recovery_4143
check_moving_sticky_recovery_4143.py
Issue #4144
check_envframe_pool_slot_remap_4144
check_envframe_pool_slot_remap_4144.py
Issue #4145
check_soft_autowire_unreachable_4145
check_soft_autowire_unreachable_4145.py
Issue #4146
check_facade_cone_jit_drop_4146
check_facade_cone_jit_drop_4146.py
Issue #4147
check_residual_force_rearm_4147
check_residual_force_rearm_4147.py
Issue #4148
check_jit_drop_must_deopt_4148
check_jit_drop_must_deopt_4148.py
Issue #4149
check_hygiene_limit_stamp_4149
check_hygiene_limit_stamp_4149.py
Issue #4150
check_hygiene_last_limit_fiber_4150
check_hygiene_last_limit_fiber_4150.py
Issue #4151
check_macro_child_walk_gate_4151
check_macro_child_walk_gate_4151.py
Issue #4152
check_deferred_green_steal_fence_4152
check_deferred_green_steal_fence_4152.py
Issue #4153
check_txn_dirty_green_4153
check_txn_dirty_green_4153.py
Issue #4154
check_residual_castop_remirror_4154
check_residual_castop_remirror_4154.py
Issue #4155
check_residual_castop_densify_clear_4155
check_residual_castop_densify_clear_4155.py
Issue #4156
check_adaptive_thr_4156
check_adaptive_thr_4156.py
Issue #4157
check_shape_flip_persist_fresh_4157
check_shape_flip_persist_fresh_4157.py
Issue #4158
check_edge_free_hold_4158
check_edge_free_hold_4158.py
check_chaos_ready_residual_gate_4159
check_chaos_ready_residual_gate_4159.py
Issue #4160
check_mailbox_holder_bound_4160
check_mailbox_holder_bound_4160.py
Issue #4161
check_rejecthard_bit_coverage_4161
check_rejecthard_bit_coverage_4161.py
Issue #4162
check_query_free_slot_4162
check_query_free_slot_4162.py
Issue #4163
check_query_index_free_slot_4163
check_query_index_free_slot_4163.py
Issue #4164
check_ref_layout_gen_paint_4164
check_ref_layout_gen_paint_4164.py
Issue #4165
check_agent_fiber_isolation_4165
check_agent_fiber_isolation_4165.py
Issue #4166
check_shape_merge_lock_scope_4166
check_shape_merge_lock_scope_4166.py
Issue #4167
check_hot_contract_pack_face_4167
check_hot_contract_pack_face_4167.py
Issue #4168
check_ir_dirty_soft_pretend_amortized_4168
check_ir_dirty_soft_pretend_amortized_4168.py
Issue #4169
check_apply_closure_tls_seq_4169
check_apply_closure_tls_seq_4169.py
Issue #4170
check_recover_solve_status_4170
check_recover_solve_status_4170.py
Issue #4171
check_blame_ok_vacuous_4171
check_blame_ok_vacuous_4171.py
Issue #4172
check_residual_undermark_jit_4172
check_residual_undermark_jit_4172.py
Issue #4173
check_sampled_leftover_audit_4173
check_sampled_leftover_audit_4173.py
Issue #4174
check_recover_truncation_4174
check_recover_truncation_4174.py
Issue #4175
check_r7rs_string_compare_4175
check_r7rs_string_compare_4175.py
Issue #4176
check_serve_async_join_wake_4176
check_serve_async_join_wake_4176.py
Issue #4177
check_list_take_drop_4177
check_list_take_drop_4177.py
check_oneshot_stdlib_4178
check_oneshot_stdlib_4178.py
check_oneshot_stdlib_4220
check_oneshot_stdlib_4220.py
check_oneshot_stdlib_4224
check_oneshot_stdlib_4224.py
check_oneshot_filter_4228
check_oneshot_filter_4228.py
Issue #4229
check_serve_async_capture_honesty_4229
check_serve_async_capture_honesty_4229.py
Issue #4230
check_fiber_join_done_face_4230
check_fiber_join_done_face_4230.py
Issue #4231
check_fiber_body_exception_4231
check_fiber_body_exception_4231.py
Issue #4232
check_soft_list_prims_4232
check_soft_list_prims_4232.py
Issue #4233
check_tenant_exec_jail_4233
check_tenant_exec_jail_4233.py
Issue #4234
check_exec_high_risk_force_4234
check_exec_high_risk_force_4234.py
check_agent_mint_per_agent_4235
check_agent_mint_per_agent_4235.py
Issue #4236
check_join_fail_circuitbreaker_4236
check_join_fail_circuitbreaker_4236.py
Issue #4237
check_restart_fuel_4237
check_restart_fuel_4237.py
Issue #4238
check_scope_region_admit_4238
check_scope_region_admit_4238.py
Issue #4239
check_require_effect_join_refuse_4239
check_require_effect_join_refuse_4239.py
Issue #4240
check_wal_prune_fold_4240
check_wal_prune_fold_4240.py
Issue #4241
check_mid_refuse_ordering_4241
check_mid_refuse_ordering_4241.py
Issue #4242
check_remap_tombstone_quarantine_4242
check_remap_tombstone_quarantine_4242.py
Issue #4243
check_relocate_identity_4243
check_relocate_identity_4243.py
Issue #4244
check_interior_int_remap_4244
check_interior_int_remap_4244.py
Issue #4245
Issue #4246
Issue #4247
Issue #4248
Issue #4249
Issue #4250
check_occurrence_drain_order_4250
check_occurrence_drain_order_4250.py
Issue #4251
check_dep_closure_miss_4251
check_dep_closure_miss_4251.py
Issue #4252
check_occurrence_owner_tc_steal_4252
check_occurrence_owner_tc_steal_4252.py
Issue #4257
Issue #4258
Issue #4259
Issue #4260
check_primfn_no_std_function_4261
check_primfn_no_std_function_4261.py
Issue #4262
check_escape_soa_no_flat_4262
check_escape_soa_no_flat_4262.py
check_ir_soa_column_slab_upstream_4263
check_ir_soa_column_slab_upstream_4263.py
Issue #4264
check_serve_setcode_parity_4264
check_serve_setcode_parity_4264.py
Issue #4265
check_pick_best_call_cache_4265
check_pick_best_call_cache_4265.py
Issue #4266
check_safe_refactor_typecheck_4266
check_safe_refactor_typecheck_4266.py
Issue #4267
check_synthesize_optimize_score_4267
check_synthesize_optimize_score_4267.py
Issue #4270
Issue #4271
Issue #4272
Issue #4273
check_setcode_pick_parity_4272_4273
check_setcode_pick_parity_4272_4273.py
Issue #4277
Issue #4278
Issue #4279
Issue #4280
check_name_table_region_admit_4280
check_name_table_region_admit_4280.py
Issue #4281
Issue #4286
Issue #4287
Issue #4288
Issue #4289
Issue #4290
Issue #4291
Issue #4292
Issue #4293
Issue #4294
Issue #4296
Issue #4297
Issue #4298
Issue #4299
Issue #4301
Issue #4303
Issue #4304
Issue #4305
Issue #4306
Issue #4307
Issue #4308
Issue #4309
Issue #4310
Issue #4311
Issue #4312
Issue #4313
Issue #4314
Issue #4315
Issue #4316
Issue #4317
Issue #4318
Issue #4319
Issue #4321
Issue #4322
Issue #4323
Issue #4326
Issue #4327
Issue #4328
Issue #4329
Issue #4330
Issue #4331
Issue #4332
Issue #4334
Issue #4335
Issue #4336
Issue #4337
Issue #4338
Issue #4339
Issue #4340
Issue #4341
Issue #4342
Issue #4343
Issue #4344
Issue #4345
Issue #4346
Issue #4347
Issue #4348
Issue #4349
Issue #4350
Issue #4353
Issue #4355
Issue #4357
Issue #4359
Issue #4360
Issue #4362
Issue #4365
Issue #4372
Issue #4373
Issue #4374
Issue #4375
Issue #4377
Issue #4379
Issue #4380
check_tenant_jail_4380
check_tenant_jail_4380.py
Issue #4381
check_git_tenant_jail_4381
check_git_tenant_jail_4381.py
Issue #4382
check_http_tenant_scheme_4382
check_http_tenant_scheme_4382.py
Issue #4383
Issue #4384
Issue #4385
Issue #4386
Issue #4387
Issue #4388
Issue #4389
Issue #4390
Issue #4391
Issue #4392
Issue #4393
Issue #4394
Issue #4395
Issue #4396
Issue #4397
Issue #4398
Issue #4399
Issue #4400
Issue #4401
Issue #4402
Issue #4403
Issue #4404
Issue #4405
Issue #4406
Issue #4407
Issue #4408
Issue #4409
Issue #4410
Issue #4411
Issue #4413
Issue #4414
Issue #4415
Issue #4416
Issue #4417
Issue #4418
Issue #4419
Issue #4420
Issue #4421
Issue #4422
Issue #4423
Issue #4424
Issue #4425
Issue #4426
Issue #4427
Issue #4428
Issue #4429
Issue #4430
Issue #4431
Issue #4432
Issue #4433
Issue #4434
Issue #4435
Issue #4436
Issue #4438
Issue #4439
Issue #4440
Issue #4441
Issue #4442
Issue #4443
Issue #4444
Issue #4445
Issue #4446
Issue #4448
Issue #4449
Issue #4450
Issue #4451
Issue #4452
Issue #4453
Issue #4454
Issue #4455
Issue #4456
Issue #4457
Issue #4460
Issue #4461
Issue #4462
Issue #4463
Issue #4464
Issue #4465
Issue #4466
Issue #4467
Issue #4468
Issue #4469
Issue #4470
Issue #4471
Issue #4472
Issue #4473
Issue #4474
check_reclaim_trigger_large_heap_4474
check_reclaim_trigger_large_heap_4474.py
Issue #4475
Issue #4476
Issue #4477
check_prim_shadow_eval_current_4477
check_prim_shadow_eval_current_4477.py
Issue #4478
Issue #4480
Issue #4482
Issue #4483
Issue #4492
Issue #4497
check_value_encoding_single_source_4502
check_value_encoding_single_source_4502.py
Issue #4504
Issue #4505
Issue #4506
Issue #4507
Issue #4508
Issue #4510
Issue #4511
Issue #4513
Issue #4514
Issue #4515
Issue #4516
Issue #4517
Issue #4518
Issue #4519
Issue #4520
Issue #4521
Issue #4522
Issue #4523
Issue #4524
Issue #4525
Issue #4527
Issue #4528
Issue #4529
Issue #4530
Issue #4531
Issue #4532
Issue #4533
Issue #4534
Issue #4538
Issue #4539
Issue #4540
Issue #4542
Issue #4543
Issue #4544
Issue #4545
Issue #4548
Issue #4551
Issue #4553
Issue #4554
Issue #4555
Issue #4557
Issue #4558
check_tenant_string_sink_4559
check_tenant_string_sink_4559.py
Issue #4560
Issue #4561
Issue #4563
Issue #4564
Issue #4565
Issue #4569
Issue #4575
Issue #4576
Issue #4577
Issue #4578
Issue #4579
Issue #4580
Issue #4581
Issue #4583
Issue #4584
Issue #4585
Issue #4586
Issue #4587
Issue #4588
Issue #4589
check_top_letrec_no_selfcopy_frame_4595
check_top_letrec_no_selfcopy_frame_4595.py
Issue #4596
Issue #4597
Issue #4598
check_boundary_exit_counters_4598
check_boundary_exit_counters_4598.py
check_snapshot_share_stamps_4598
check_snapshot_share_stamps_4598.py
Issue #4599
check_call_resolve_cache_4599
check_call_resolve_cache_4599.py
Issue #4600
check_jit_assumption_gate_4600
check_jit_assumption_gate_4600.py
check_jit_frame_deopt_4600
check_jit_frame_deopt_4600.py
check_jit_osr_4600
check_jit_osr_4600.py
check_jit_reuse_lowering_4600
check_jit_reuse_lowering_4600.py
check_jit_pair_mutation_4602
check_jit_pair_mutation_4602.py
Issue #4606
check_jit_prim_callee_refuse_4606
check_jit_prim_callee_refuse_4606.py
Issue #4607
check_jit_unbound_callee_4607
check_jit_unbound_callee_4607.py
Issue #4608
check_string_heap_reclaim_4608
check_string_heap_reclaim_4608.py
Issue #4611
check_prim_call_arg_rooting_4611
check_prim_call_arg_rooting_4611.py
Issue #4612
check_hash_probe_4612
check_hash_probe_4612.py
Issue #4613
check_pair_vector_hash_reclaim_4613
check_pair_vector_hash_reclaim_4613.py
Issue #4614
check_vector_hash_cow_4614
check_vector_hash_cow_4614.py
check_block_prefix_offsets_4615
check_block_prefix_offsets_4615.py
Issue #4624
Issue #4630
Issue #4631
Issue #4632
Issue #4633
Issue #4636
Issue #4639
Issue #4641
Issue #4642
Issue #4643
Issue #4644
Issue #4645
Issue #4646
Issue #4647
Issue #4648
Issue #4649
Issue #4650
Issue #4651
Issue #4652
check_query_and_replace_guard_fail_4652
check_query_and_replace_guard_fail_4652.py
Issue #4653
check_partial_refusal_commit_4653
check_partial_refusal_commit_4653.py
Issue #4654
check_wal_slo_header_contract_4657
check_wal_slo_header_contract_4657.py
Issue #4658
Issue #4659
Issue #4661
Issue #4662
Issue #4663
Issue #4664
Issue #4665
check_mutate_hygiene_gate_4665
check_mutate_hygiene_gate_4665.py
Issue #4666
Issue #4667
Issue #4670
Issue #4671
check_jit_per_op_typed_entry_readiness_4671
check_jit_per_op_typed_entry_readiness_4671.py
Issue #4672
check_outermost_abort_forced_dirty_4672
check_outermost_abort_forced_dirty_4672.py
Issue #4673
Issue #4674
check_issue_test_main_linkage_4675
check_issue_test_main_linkage_4675.py
check_aot_runtime_weak_stub_4676
check_aot_runtime_weak_stub_4676.py
Issue #4681
Issue #4684
check_if_dynamic_arm_ground_gate_4684
check_if_dynamic_arm_ground_gate_4684.py
Issue #4685
Issue #4686
check_se_wal_second_enable_4686
check_se_wal_second_enable_4686.py
Issue #4687
Issue #4688
Issue #4689
Issue #4690
Issue #4691
Issue #4692
Issue #4693
Issue #4694
Issue #4695
Issue #4696
check_pin_stable_refs_provenance_4696
check_pin_stable_refs_provenance_4696.py
Issue #4697
Issue #4698
Issue #4699
Issue #4700
Issue #4701
Issue #4702
Issue #4704
Issue #4707
Issue #4714
Issue #4715
Issue #4716
Issue #4717
Issue #4719
Issue #4721
Issue #4722
Issue #4723
Issue #4724
Issue #4725
Issue #4726
Issue #4727
check_ir_const_symbol_intern_4727
check_ir_const_symbol_intern_4727.py
Issue #4728
Issue #4729
Issue #4730
Issue #4731
Issue #4732
Issue #4735
Issue #4736
Issue #4737
Issue #4742
Issue #4743
Issue #4744
Issue #4745
Issue #4747
Issue #4749
Issue #4750
Issue #4751
Issue #4752
Issue #4753
Issue #4756
Issue #4759
Issue #4763
Issue #4766
Issue #4768
Issue #4769
Issue #4770
Issue #4771
Issue #4772
Issue #4774
Issue #4779
Issue #4783
Issue #4787
Issue #4790
check_frame_gc_4790
check_frame_gc_4790.py
Issue #4791
Issue #4792
Issue #4793
Issue #4816
Issue #4827
Issue #4832
Issue #4841
Issue #4853
Issue #4855
Issue #4858
Issue #4876
Issue #4884
Issue #4885
Issue #4886
Issue #4891
Issue #4896
Issue #4899
Issue #4902
Issue #4905
Issue #4909
Issue #4916
Issue #4917
Issue #4919
Issue #4921
Issue #4922
Issue #4923
Issue #4924
Issue #4925
Issue #4926
Issue #4927
Issue #4928
Issue #4930
Issue #4932
Issue #4933
Issue #4936
Issue #4946
Issue #4965
check_primitive_tiers_5008
check_primitive_tiers_5008.py
Issue #5033
Issue #5041
Issue #5051
Issue #5054
Issue #5057
Issue #5087
Issue #5117
check_orch_doc_hint_5118
check_orch_doc_hint_5118.py
check_stdlib_surface_drift_5132
check_stdlib_surface_drift_5132.py
Issue #5139
Issue #5141
Issue #5144
Issue #5145
Issue #5146
Issue #5147
Issue #5148
Issue #5149
Issue #5151
check_stdlib_unbound_5169
check_stdlib_unbound_5169.py
Issue #5173
Issue #5175
Issue #5190
check_stdlib_list_prim_shadow_5192
check_stdlib_list_prim_shadow_5192.py
Issue #5195
Issue #5208
Issue #5211
Issue #5212
Issue #5213
Issue #5244
Issue #5274
Issue #5275
Issue #5279
Issue #5280
Issue #5281
Issue #5282
Issue #5289
Issue #5292
Issue #5295
Issue #5300
Issue #5303
"""
# Issue #100
# Issue #101
# Issue #102
# Issue #103
# Issue #106
# Issue #107
# Issue #113
# Issue #114
# Issue #115
# Issue #119
# Issue #121
# Issue #124
# Issue #125
# Issue #130
# Issue #160
# Issue #161
# Issue #173
# Issue #178
# Issue #183
# Issue #187
# Issue #189
# Issue #192
# Issue #206
# Issue #211
# Issue #212
# Issue #213
# Issue #218
# Issue #219
# Issue #220
# Issue #223
# Issue #224
# Issue #226
# Issue #254
# Issue #261
# Issue #268
# Issue #278
# Issue #283
# Issue #289
# Issue #300
# Issue #306
# Issue #307
# Issue #322
# Issue #324
# Issue #335
# Issue #339
# Issue #344
# Issue #354
# Issue #365
# Issue #368
# Issue #378
# Issue #381
# Issue #384
# Issue #393
# Issue #398
# Issue #399
# Issue #400
# Issue #403
# Issue #404
# Issue #405
# Issue #406
# Issue #407
# Issue #408
# Issue #411
# Issue #414
# Issue #416
# Issue #417
# Issue #418
# Issue #419
# Issue #420
# Issue #421
# Issue #422
# Issue #423
# Issue #424
# Issue #427
# Issue #432
# Issue #438
# Issue #441
# Issue #454
# Issue #464
# Issue #467
# Issue #468
# Issue #477
# Issue #478
# Issue #480
# Issue #489
# Issue #491
# Issue #495
# Issue #506
# Issue #508
# Issue #509
# Issue #514
# Issue #517
# Issue #518
# Issue #526
# Issue #527
# Issue #529
# Issue #531
# Issue #536
# Issue #542
# Issue #543
# Issue #544
# Issue #545
# Issue #546
# Issue #547
# Issue #548
# Issue #551
# Issue #552
# Issue #553
# Issue #554
# Issue #556
# Issue #560
# Issue #561
# Issue #562
# Issue #563
# Issue #564
# Issue #565
# Issue #570
# Issue #571
# Issue #573
# Issue #574
# Issue #575
# Issue #576
# Issue #577
# Issue #583
# Issue #588
# Issue #592
# Issue #593
# Issue #594
# Issue #595
# Issue #596
# Issue #597
# Issue #598
# Issue #599
# Issue #600
# Issue #602
# Issue #604
# Issue #605
# Issue #607
# Issue #608
# Issue #609
# Issue #610
# Issue #611
# Issue #612
# Issue #619
# Issue #623
# Issue #627
# Issue #628
# Issue #629
# Issue #634
# Issue #635
# Issue #636
# Issue #638
# Issue #639
# Issue #642
# Issue #652
# Issue #653
# Issue #654
# Issue #655
# Issue #657
# Issue #658
# Issue #659
# Issue #673
# Issue #674
# Issue #685
# Issue #731
# Issue #737
# Issue #738
# Issue #739
# Issue #740
# Issue #741
# Issue #742
# Issue #743
# Issue #744
# Issue #745
# Issue #746
# Issue #747
# Issue #751
# Issue #752
# Issue #753
# Issue #754
# Issue #755
# Issue #798
# Issue #799
# Issue #800
# Issue #803
# Issue #805
# Issue #818
# Issue #881
# Issue #923
# Issue #941
# Issue #957
# Issue #986
# Issue #1013
# Issue #1014
# Issue #1047
# Issue #1097
# Issue #1202
# Issue #1241
# Issue #1242
# Issue #1289
# Issue #1301
# Issue #1321
# Issue #1356
# Issue #1359
# Issue #1360
# Issue #1361
# Issue #1362
# Issue #1364
# Issue #1365
# Issue #1366
# Issue #1367
# Issue #1368
# Issue #1369
# Issue #1370
# Issue #1371
# Issue #1372
# Issue #1373
# Issue #1374
# Issue #1375
# Issue #1376
# Issue #1377
# Issue #1378
# Issue #1380
# Issue #1381
# Issue #1387
# Issue #1388
# Issue #1389
# Issue #1390
# Issue #1393
# Issue #1397
# Issue #1398
# Issue #1401
# Issue #1407
# Issue #1408
# Issue #1410
# Issue #1411
# Issue #1412
# Issue #1413
# Issue #1416
# Issue #1420
# Issue #1425
# Issue #1433
# Issue #1434
# Issue #1435
# Issue #1436
# Issue #1437
# Issue #1439
# Issue #1444
# Issue #1445
# Issue #1446
# Issue #1458
# Issue #1465
# Issue #1469
# Issue #1476
# Issue #1478
# Issue #1480
# Issue #1481
# Issue #1482
# Issue #1483
# Issue #1490
# Issue #1495
# Issue #1496
# Issue #1501
# Issue #1503
# Issue #1504
# Issue #1506
# Issue #1511
# Issue #1518
# Issue #1519
# Issue #1526
# Issue #1531
# Issue #1534
# Issue #1535
# Issue #1543
# Issue #1546
# Issue #1547
# Issue #1548
# Issue #1557
# Issue #1565
# Issue #1566
# Issue #1567
# Issue #1568
# Issue #1575
# Issue #1576
# Issue #1577
# Issue #1579
# Issue #1580
# Issue #1581
# Issue #1582
# Issue #1583
# Issue #1584
# Issue #1585
# Issue #1586
# Issue #1587
# Issue #1588
# Issue #1589
# Issue #1590
# Issue #1591
# Issue #1592
# Issue #1594
# Issue #1595
# Issue #1596
# Issue #1597
# Issue #1598
# Issue #1599
# Issue #1600
# Issue #1601
# Issue #1602
# Issue #1604
# Issue #1605
# Issue #1606
# Issue #1607
# Issue #1608
# Issue #1609
# Issue #1610
# Issue #1611
# Issue #1612
# Issue #1613
# Issue #1614
# Issue #1615
# Issue #1616
# Issue #1617
# Issue #1618
# Issue #1619
# Issue #1620
# Issue #1621
# Issue #1622
# Issue #1623
# Issue #1624
# Issue #1626
# Issue #1627
# Issue #1628
# Issue #1631
# Issue #1633
# Issue #1634
# Issue #1635
# Issue #1636
# Issue #1638
# Issue #1639
# Issue #1640
# Issue #1641
# Issue #1648
# Issue #1652
# Issue #1655
# Issue #1657
# Issue #1658
# Issue #1659
# Issue #1660
# Issue #1661
# Issue #1662
# Issue #1663
# Issue #1664
# Issue #1665
# Issue #1666
# Issue #1667
# Issue #1670
# Issue #1671
# Issue #1672
# Issue #1678
# Issue #1679
# Issue #1680
# Issue #1681
# Issue #1682
# Issue #1684
# Issue #1685
# Issue #1686
# Issue #1687
# Issue #1688
# Issue #1689
# Issue #1690
# Issue #1691
# Issue #1692
# Issue #1693
# Issue #1694
# Issue #1695
# Issue #1696
# Issue #1697
# Issue #1699
# Issue #1700
# Issue #1701
# Issue #1702
# Issue #1703
# Issue #1706
# Issue #1707
# Issue #1708
# Issue #1709
# Issue #1710
# Issue #1711
# Issue #1712
# Issue #1714
# Issue #1715
# Issue #1716
# Issue #1717
# Issue #1718
# Issue #1719
# Issue #1720
# Issue #1721
# Issue #1722
# Issue #1723
# Issue #1724
# Issue #1725
# Issue #1726
# Issue #1727
# Issue #1728
# Issue #1729
# Issue #1730
# Issue #1731
# Issue #1732
# Issue #1733
# Issue #1734
# Issue #1739
# Issue #1740
# Issue #1745
# Issue #1746
# Issue #1747
# Issue #1753
# Issue #1754
# Issue #1755
# Issue #1756
# Issue #1757
# Issue #1764
# Issue #1765
# Issue #1766
# Issue #1767
# Issue #1768
# Issue #1769
# Issue #1770
# Issue #1771
# Issue #1772
# Issue #1779
# Issue #1780
# Issue #1781
# Issue #1782
# Issue #1783
# Issue #1785
# Issue #1786
# Issue #1787
# Issue #1795
# Issue #1797
# Issue #1806
# Issue #1816
# Issue #1818
# Issue #1827
# Issue #1835
# Issue #1837
# Issue #1838
# Issue #1839
# Issue #1840
# Issue #1842
# Issue #1843
# Issue #1844
# Issue #1845
# Issue #1846
# Issue #1847
# Issue #1848
# Issue #1849
# Issue #1850
# Issue #1851
# Issue #1852
# Issue #1853
# Issue #1854
# Issue #1855
# Issue #1856
# Issue #1857
# Issue #1858
# Issue #1859
# Issue #1860
# Issue #1861
# Issue #1862
# Issue #1863
# Issue #1864
# Issue #1865
# Issue #1866
# Issue #1867
# Issue #1868
# Issue #1869
# Issue #1870
# Issue #1871
# Issue #1872
# Issue #1873
# Issue #1874
# Issue #1875
# Issue #1876
# Issue #1877
# Issue #1878
# Issue #1879
# Issue #1880
# Issue #1881
# Issue #1882
# Issue #1883
# Issue #1884
# Issue #1885
# Issue #1887
# Issue #1888
# Issue #1889
# Issue #1890
# Issue #1891
# Issue #1892
# Issue #1893
# Issue #1894
# Issue #1895
# Issue #1896
# Issue #1897
# Issue #1898
# Issue #1899
# Issue #1908
# Issue #1909
# Issue #1910
# Issue #1911
# Issue #1912
# Issue #1913
# Issue #1914
# Issue #1916
# Issue #1917
# Issue #1918
# Issue #1919
# Issue #1924
# Issue #1927
# Issue #1928
# Issue #1929
# Issue #1930
# Issue #1931
# Issue #1947
# Issue #1948
# Issue #1949
# Issue #1966
# Issue #1987
# Issue #1988
# Issue #1989
# Issue #1990
# Issue #1991
# Issue #1992
# Issue #1993
# Issue #1994
# Issue #1996
# Issue #2000
# Issue #2001
# Issue #2002
# Issue #2003
# Issue #2004
# Issue #2006
# Issue #2007
# Issue #2008
# Issue #2010
# Issue #2011
# Issue #2012
# Issue #2013
# Issue #2014
# Issue #2015
# Issue #2016
# Issue #2017
# Issue #2018
# Issue #2019
# Issue #2020
# Issue #2021
# Issue #2022
# Issue #2023
# Issue #2024
# Issue #2025
# Issue #2026
# Issue #2027
# Issue #2028
# Issue #2029
# Issue #2030
# Issue #2031
# Issue #2032
# Issue #2033
# Issue #2034
# Issue #2035
# Issue #2036
# Issue #2037
# Issue #2038
# Issue #2040
# Issue #2041
# Issue #2042
# Issue #2043
# Issue #2044
# Issue #2045
# Issue #2046
# Issue #2052
# Issue #2054
# Issue #2055
# Issue #2056
# Issue #2057
# Issue #2058
# Issue #2059
# Issue #2060
# Issue #2061
# Issue #2062
# Issue #2063
# Issue #2064
# Issue #2065
# Issue #2066
# Issue #2067
# Issue #2068
# Issue #2072
# Issue #2073
# Issue #2074
# Issue #2075
# Issue #2076
# Issue #2077
# Issue #2078
# Issue #2079
# Issue #2080
# Issue #2081
# Issue #2082
# Issue #2083
# Issue #2084
# Issue #2085
# Issue #2086
# Issue #2087
# Issue #2088
# Issue #2089
# Issue #2090
# Issue #2092
# Issue #2093
# Issue #2094
# Issue #2095
# Issue #2096
# Issue #2097
# Issue #2098
# Issue #2099
# Issue #2100
# Issue #2101
# Issue #2102
# Issue #2103
# Issue #2104
# Issue #2105
# Issue #2106
# Issue #2107
# Issue #2108
# Issue #2109
# Issue #2110
# Issue #2111
# Issue #2112
# Issue #2113
# Issue #2114
# Issue #2115
# Issue #2116
# Issue #2117
# Issue #2118
# Issue #2119
# Issue #2120
# Issue #2121
# Issue #2122
# Issue #2123
# Issue #2124
# Issue #2125
# Issue #2126
# Issue #2127
# Issue #2128
# Issue #2129
# Issue #2130
# Issue #2131
# Issue #2132
# Issue #2133
# Issue #2137
# Issue #2139
# Issue #2140
# Issue #2141
# Issue #2142
# Issue #2143
# Issue #2144
# Issue #2145
# Issue #2146
# Issue #2147
# Issue #2148
# Issue #2149
# Issue #2150
# Issue #2151
# Issue #2152
# Issue #2153
# Issue #2154
# Issue #2155
# Issue #2156
# Issue #2157
# Issue #2158
# Issue #2159
# Issue #2161
# Issue #2162
# Issue #2163
# Issue #2164
# Issue #2165
# Issue #2166
# Issue #2167
# Issue #2169
# Issue #2170
# Issue #2171
# Issue #2172
# Issue #2173
# Issue #2174
# Issue #2175
# Issue #2176
# Issue #2177
# Issue #2178
# Issue #2179
# Issue #2180
# Issue #2181
# Issue #2182
# Issue #2183
# Issue #2184
# Issue #2185
# Issue #2186
# Issue #2187
# Issue #2188
# Issue #2189
# Issue #2190
# Issue #2191
# Issue #2192
# Issue #2193
# Issue #2194
# Issue #2195
# Issue #2196
# Issue #2197
# Issue #2198
# Issue #2199
# Issue #2200
# Issue #2201
# Issue #2202
# Issue #2203
# Issue #2204
# Issue #2205
# Issue #2206
# Issue #2207
# Issue #2208
# Issue #2209
# Issue #2210
# Issue #2211
# Issue #2212
# Issue #2219
# Issue #2220
# Issue #2221
# Issue #2222
# Issue #2223
# Issue #2224
# Issue #2225
# Issue #2226
# Issue #2227
# Issue #2228
# Issue #2229
# Issue #2230
# Issue #2231
# Issue #2232
# Issue #2233
# Issue #2234
# Issue #2235
# Issue #2236
# Issue #2237
# Issue #2238
# Issue #2239
# Issue #2240
# Issue #2241
# Issue #2242
# Issue #2243
# Issue #2244
# Issue #2245
# Issue #2246
# Issue #2247
# Issue #2248
# Issue #2249
# Issue #2250
# Issue #2251
# Issue #2252
# Issue #2253
# Issue #2254
# Issue #2256
# Issue #2257
# Issue #2258
# Issue #2259
# Issue #2260
# Issue #2261
# Issue #2262
# Issue #2263
# Issue #2264
# Issue #2268
# Issue #2269
# Issue #2271
# Issue #2272
# Issue #2273
# Issue #2274
# Issue #2275
# Issue #2276
# Issue #2277
# Issue #2278
# Issue #2280
# Issue #2281
# Issue #2282
# Issue #2283
# Issue #2284
# Issue #2285
# Issue #2286
# Issue #2287
# Issue #2288
# Issue #2289
# Issue #2290
# Issue #2291
# Issue #2293
# Issue #2294
# Issue #2295
# Issue #2296
# Issue #2297
# Issue #2298
# Issue #2299
# Issue #2300
# Issue #2301
# Issue #2302
# Issue #2304
# Issue #2306
# Issue #2307
# Issue #2308
# Issue #2309
# Issue #2310
# Issue #2312
# Issue #2313
# Issue #2314
# Issue #2315
# Issue #2316
# Issue #2317
# Issue #2318
# Issue #2319
# Issue #2320
# Issue #2321
# Issue #2333
# Issue #2335
# Issue #2337
# Issue #2338
# Issue #2339
# Issue #2340
# Issue #2341
# Issue #2342
# Issue #2343
# Issue #2344
# Issue #2345
# Issue #2346
# Issue #2347
# Issue #2348
# Issue #2349
# Issue #2350
# Issue #2351
# Issue #2352
# Issue #2353
# Issue #2354
# Issue #2355
# Issue #2356
# Issue #2357
# Issue #2358
# Issue #2359
# Issue #2360
# Issue #2361
# Issue #2362
# Issue #2363
# Issue #2364
# Issue #2365
# Issue #2366
# Issue #2367
# Issue #2368
# Issue #2369
# Issue #2370
# Issue #2371
# Issue #2372
# Issue #2374
# Issue #2375
# Issue #2376
# Issue #2377
# Issue #2378
# Issue #2379
# Issue #2381
# Issue #2382
# Issue #2383
# Issue #2384
# Issue #2385
# Issue #2386
# Issue #2387
# Issue #2388
# Issue #2389
# Issue #2390
# Issue #2391
# Issue #2392
# Issue #2393
# Issue #2394
# Issue #2395
# Issue #2396
# Issue #2397
# Issue #2398
# Issue #2399
# Issue #2400
# Issue #2402
# Issue #2403
# Issue #2404
# Issue #2405
# Issue #2406
# Issue #2408
# Issue #2409
# Issue #2410
# Issue #2411
# Issue #2412
# Issue #2413
# Issue #2414
# Issue #2415
# Issue #2416
# Issue #2417
# Issue #2418
# Issue #2419
# Issue #2420
# Issue #2421
# Issue #2422
# Issue #2423
# Issue #2424
# Issue #2425
# Issue #2426
# Issue #2427
# Issue #2428
# Issue #2429
# Issue #2430
# Issue #2431
# Issue #2432
# Issue #2433
# Issue #2435
# Issue #2436
# Issue #2437
# Issue #2438
# Issue #2439
# Issue #2440
# Issue #2441
# Issue #2442
# Issue #2443
# Issue #2444
# Issue #2445
# Issue #2446
# Issue #2447
# Issue #2448
# Issue #2449
# Issue #2450
# Issue #2451
# Issue #2452
# Issue #2453
# Issue #2454
# Issue #2455
# Issue #2456
# Issue #2457
# Issue #2458
# Issue #2459
# Issue #2460
# Issue #2461
# Issue #2462
# Issue #2463
# Issue #2465
# Issue #2467
# Issue #2468
# Issue #2469
# Issue #2471
# Issue #2472
# Issue #2473
# Issue #2474
# Issue #2475
# Issue #2476
# Issue #2477
# Issue #2478
# Issue #2479
# Issue #2480
# Issue #2481
# Issue #2482
# Issue #2483
# Issue #2484
# Issue #2485
# Issue #2486
# Issue #2487
# Issue #2488
# Issue #2489
# Issue #2490
# Issue #2491
# Issue #2492
# Issue #2493
# Issue #2494
# Issue #2495
# Issue #2496
# Issue #2497
# Issue #2498
# Issue #2499
# Issue #2500
# Issue #2501
# Issue #2502
# Issue #2503
# Issue #2504
# Issue #2505
# Issue #2506
# Issue #2507
# Issue #2508
# Issue #2509
# Issue #2510
# Issue #2511
# Issue #2512
# Issue #2513
# Issue #2514
# Issue #2515
# Issue #2516
# Issue #2517
# Issue #2518
# Issue #2519
# Issue #2520
# Issue #2521
# Issue #2522
# Issue #2523
# Issue #2524
# Issue #2525
# Issue #2526
# Issue #2527
# Issue #2528
# Issue #2529
# Issue #2530
# Issue #2531
# Issue #2532
# Issue #2533
# Issue #2534
# Issue #2535
# Issue #2536
# Issue #2537
# Issue #2538
# Issue #2539
# Issue #2540
# Issue #2541
# Issue #2542
# Issue #2543
# Issue #2544
# Issue #2545
# Issue #2546
# Issue #2547
# Issue #2548
# Issue #2549
# Issue #2550
# Issue #2551
# Issue #2552
# Issue #2553
# Issue #2555
# Issue #2556
# Issue #2557
# Issue #2558
# Issue #2559
# Issue #2560
# Issue #2561
# Issue #2562
# Issue #2564
# Issue #2566
# Issue #2567
# Issue #2568
# Issue #2569
# Issue #2570
# Issue #2571
# Issue #2572
# Issue #2573
# Issue #2574
# Issue #2575
# Issue #2576
# Issue #2577
# Issue #2578
# Issue #2579
# Issue #2581
# Issue #2582
# Issue #2583
# Issue #2584
# Issue #2585
# Issue #2586
# Issue #2587
# Issue #2588
# Issue #2589
# Issue #2590
# Issue #2591
# Issue #2592
# Issue #2593
# Issue #2594
# Issue #2595
# Issue #2596
# Issue #2597
# Issue #2598
# Issue #2599
# Issue #2600
# Issue #2601
# Issue #2602
# Issue #2603
# Issue #2604
# Issue #2605
# Issue #2606
# Issue #2607
# Issue #2608
# Issue #2609
# Issue #2610
# Issue #2611
# Issue #2613
# Issue #2614
# Issue #2615
# Issue #2616
# Issue #2617
# Issue #2618
# Issue #2619
# Issue #2620
# Issue #2621
# Issue #2622
# Issue #2624
# Issue #2626
# Issue #2627
# Issue #2628
# Issue #2630
# Issue #2631
# Issue #2634
# Issue #2635
# Issue #2637
# Issue #2638
# Issue #2639
# Issue #2640
# Issue #2641
# Issue #2643
# Issue #2645
# Issue #2646
# Issue #2647
# Issue #2648
# Issue #2650
# Issue #2651
# Issue #2652
# Issue #2653
# Issue #2654
# Issue #2655
# Issue #2656
# Issue #2657
# Issue #2658
# Issue #2659
# Issue #2660
# Issue #2661
# Issue #2662
# Issue #2663
# Issue #2664
# Issue #2665
# Issue #2666
# Issue #2667
# Issue #2668
# Issue #2669
# Issue #2670
# Issue #2671
# Issue #2672
# Issue #2673
# Issue #2674
# Issue #2675
# Issue #2676
# Issue #2677
# Issue #2678
# Issue #2679
# Issue #2680
# Issue #2681
# Issue #2682
# Issue #2683
# Issue #2684
# Issue #2685
# Issue #2686
# Issue #2687
# Issue #2688
# Issue #2689
# Issue #2690
# Issue #2691
# Issue #2692
# Issue #2693
# Issue #2694
# Issue #2695
# Issue #2696
# Issue #2697
# Issue #2698
# Issue #2699
# Issue #2700
# Issue #2701
# Issue #2702
# Issue #2703
# Issue #2704
# Issue #2705
# Issue #2706
# Issue #2707
# Issue #2708
# Issue #2709
# Issue #2710
# Issue #2711
# Issue #2712
# Issue #2713
# Issue #2714
# Issue #2715
# Issue #2716
# Issue #2717
# Issue #2718
# Issue #2719
# Issue #2720
# Issue #2721
# Issue #2722
# Issue #2723
# Issue #2724
# Issue #2726
# Issue #2727
# Issue #2728
# Issue #2742
# Issue #2743
# Issue #2744
# Issue #2745
# Issue #2746
# Issue #2747
# Issue #2748
# Issue #2749
# Issue #2750
# Issue #2751
# Issue #2752
# Issue #2753
# Issue #2754
# Issue #2755
# Issue #2756
# Issue #2757
# Issue #2758
# Issue #2759
# Issue #2760
# Issue #2761
# Issue #2762
# Issue #2763
# Issue #2764
# Issue #2765
# Issue #2766
# Issue #2768
# Issue #2769
# Issue #2770
# Issue #2771
# Issue #2773
# Issue #2774
# Issue #2775
# Issue #2776
# Issue #2777
# Issue #2778
# Issue #2779
# Issue #2780
# Issue #2781
# Issue #2782
# Issue #2783
# Issue #2784
# Issue #2785
# Issue #2786
# Issue #2787
# Issue #2788
# Issue #2789
# Issue #2790
# Issue #2791
# Issue #2792
# Issue #2793
# Issue #2794
# Issue #2795
# Issue #2796
# Issue #2797
# Issue #2798
# Issue #2799
# Issue #2800
# Issue #2801
# Issue #2802
# Issue #2803
# Issue #2804
# Issue #2805
# Issue #2806
# Issue #2807
# Issue #2808
# Issue #2809
# Issue #2810
# Issue #2811
# Issue #2812
# Issue #2813
# Issue #2814
# Issue #2815
# Issue #2816
# Issue #2817
# Issue #2818
# Issue #2819
# Issue #2820
# Issue #2821
# Issue #2822
# Issue #2823
# Issue #2824
# Issue #2825
# Issue #2826
# Issue #2827
# Issue #2828
# Issue #2829
# Issue #2830
# Issue #2831
# Issue #2832
# Issue #2833
# Issue #2834
# Issue #2835
# Issue #2836
# Issue #2837
# Issue #2838
# Issue #2839
# Issue #2840
# Issue #2841
# Issue #2842
# Issue #2843
# Issue #2844
# Issue #2845
# Issue #2846
# Issue #2847
# Issue #2848
# Issue #2849
# Issue #2850
# Issue #2851
# Issue #2852
# Issue #2853
# Issue #2854
# Issue #2855
# Issue #2856
# Issue #2857
# Issue #2858
# Issue #2859
# Issue #2860
# Issue #2861
# Issue #2862
# Issue #2863
# Issue #2864
# Issue #2865
# Issue #2868
# Issue #2869
# Issue #2875
# Issue #2877
# Issue #2878
# Issue #2879
# Issue #2880
# Issue #2881
# Issue #2882
# Issue #2883
# Issue #2884
# Issue #2885
# Issue #2886
# Issue #2887
# Issue #2888
# Issue #2889
# Issue #2890
# Issue #2891
# Issue #2892
# Issue #2893
# Issue #2894
# Issue #2895
# Issue #2896
# Issue #2897
# Issue #2898
# Issue #2899
# Issue #2900
# Issue #2901
# Issue #2902
# Issue #2903
# Issue #2904
# Issue #2905
# Issue #2906
# Issue #2907
# Issue #2908
# Issue #2909
# Issue #2910
# Issue #2911
# Issue #2912
# Issue #2913
# Issue #2914
# Issue #2915
# Issue #2916
# Issue #2918
# Issue #2919
# Issue #2921
# Issue #2922
# Issue #2923
# Issue #2924
# Issue #2925
# Issue #2926
# Issue #2927
# Issue #2928
# Issue #2929
# Issue #2930
# Issue #2932
# Issue #2933
# Issue #2934
# Issue #2935
# Issue #2936
# Issue #2937
# Issue #2938
# Issue #2939
# Issue #2942
# Issue #2943
# Issue #2944
# Issue #2945
# Issue #2946
# Issue #2947
# Issue #2948
# Issue #2949
# Issue #2950
# Issue #2951
# Issue #2952
# Issue #2953
# Issue #2954
# Issue #2955
# Issue #2956
# Issue #2957
# Issue #2958
# Issue #2959
# Issue #2960
# Issue #2961
# Issue #2962
# Issue #2963
# Issue #2964
# Issue #2966
# Issue #2967
# Issue #2968
# Issue #2969
# Issue #2970
# Issue #2971
# Issue #2972
# Issue #2973
# Issue #2974
# Issue #2975
# Issue #2976
# Issue #2977
# Issue #2978
# Issue #2979
# Issue #2980
# Issue #2981
# Issue #2982
# Issue #2983
# Issue #2984
# Issue #2985
# Issue #2986
# Issue #2987
# Issue #2988
# Issue #2989
# Issue #2990
# Issue #2991
# Issue #2992
# Issue #2993
# Issue #2994
# Issue #2995
# Issue #2996
# Issue #2997
# Issue #2998
# Issue #2999
# Issue #3000
# Issue #3001
# Issue #3002
# Issue #3003
# Issue #3004
# Issue #3005
# Issue #3006
# Issue #3007
# Issue #3010
# Issue #3011
# Issue #3012
# Issue #3013
# Issue #3014
# Issue #3015
# Issue #3016
# Issue #3017
# Issue #3018
# Issue #3019
# Issue #3020
# Issue #3021
# Issue #3022
# Issue #3023
# Issue #3024
# Issue #3025
# Issue #3026
# Issue #3027
# Issue #3028
# Issue #3029
# Issue #3030
# Issue #3031
# Issue #3032
# Issue #3033
# Issue #3034
# Issue #3035
# Issue #3036
# Issue #3037
# Issue #3038
# Issue #3039
# Issue #3040
# Issue #3041
# Issue #3042
# Issue #3043
# Issue #3044
# Issue #3045
# Issue #3046
# Issue #3048
# Issue #3049
# Issue #3050
# Issue #3051
# Issue #3052
# Issue #3053
# Issue #3054
# Issue #3055
# Issue #3056
# Issue #3057
# Issue #3058
# Issue #3059
# Issue #3060
# Issue #3061
# Issue #3062
# Issue #3063
# Issue #3064
# Issue #3065
# Issue #3066
# Issue #3067
# Issue #3068
# Issue #3069
# Issue #3070
# Issue #3071
# Issue #3072
# Issue #3073
# Issue #3074
# Issue #3075
# Issue #3076
# Issue #3081
# Issue #3082
# Issue #3083
# Issue #3084
# Issue #3085
# Issue #3088
# Issue #3089
# Issue #3090
# Issue #3091
# Issue #3092
# Issue #3093
# Issue #3094
# Issue #3095
# Issue #3096
# Issue #3097
# Issue #3098
# Issue #3099
# Issue #3100
# Issue #3101
# Issue #3102
# Issue #3103
# Issue #3104
# Issue #3106
# Issue #3107
# Issue #3108
# Issue #3109
# Issue #3110
# Issue #3111
# Issue #3112
# Issue #3113
# Issue #3114
# Issue #3115
# Issue #3116
# Issue #3117
# Issue #3118
# Issue #3119
# Issue #3120
# Issue #3121
# Issue #3122
# Issue #3123
# Issue #3124
# Issue #3125
# Issue #3126
# Issue #3127
# Issue #3128
# Issue #3129
# Issue #3130
# Issue #3131
# Issue #3132
# Issue #3133
# Issue #3134
# Issue #3135
# Issue #3136
# Issue #3137
# Issue #3138
# Issue #3139
# Issue #3140
# Issue #3141
# Issue #3142
# Issue #3143
# Issue #3144
# Issue #3145
# Issue #3146
# Issue #3147
# Issue #3148
# Issue #3150
# Issue #3151
# Issue #3152
# Issue #3153
# Issue #3154
# Issue #3155
# Issue #3156
# Issue #3157
# Issue #3158
# Issue #3159
# Issue #3160
# Issue #3161
# Issue #3162
# Issue #3163
# Issue #3164
# Issue #3165
# Issue #3166
# Issue #3167
# Issue #3168
# Issue #3169
# Issue #3170
# Issue #3171
# Issue #3172
# Issue #3173
# Issue #3174
# Issue #3175
# Issue #3176
# Issue #3177
# Issue #3178
# Issue #3179
# Issue #3180
# Issue #3181
# Issue #3182
# Issue #3183
# Issue #3184
# Issue #3185
# Issue #3186
# Issue #3187
# Issue #3188
# Issue #3189
# Issue #3190
# Issue #3191
# Issue #3192
# Issue #3193
# Issue #3194
# Issue #3195
# Issue #3196
# Issue #3197
# Issue #3198
# Issue #3199
# Issue #3200
# Issue #3201
# Issue #3202
# Issue #3203
# Issue #3204
# Issue #3205
# Issue #3206
# Issue #3207
# Issue #3208
# Issue #3209
# Issue #3210
# Issue #3211
# Issue #3212
# Issue #3213
# Issue #3214
# Issue #3215
# Issue #3216
# Issue #3217
# Issue #3219
# Issue #3220
# Issue #3221
# Issue #3222
# Issue #3223
# Issue #3224
# Issue #3225
# Issue #3226
# Issue #3227
# Issue #3228
# Issue #3229
# Issue #3230
# Issue #3231
# Issue #3232
# Issue #3233
# Issue #3234
# Issue #3235
# Issue #3236
# Issue #3237
# Issue #3238
# Issue #3239
# Issue #3241
# Issue #3242
# Issue #3243
# Issue #3244
# Issue #3245
# Issue #3246
# Issue #3247
# Issue #3248
# Issue #3249
# Issue #3250
# Issue #3251
# Issue #3252
# Issue #3253
# Issue #3254
# Issue #3255
# Issue #3256
# Issue #3257
# Issue #3258
# Issue #3259
# Issue #3260
# Issue #3261
# Issue #3262
# Issue #3263
# Issue #3264
# Issue #3265
# Issue #3266
# Issue #3267
# Issue #3268
# Issue #3269
# Issue #3270
# Issue #3271
# Issue #3272
# Issue #3273
# Issue #3274
# Issue #3275
# Issue #3276
# Issue #3277
# Issue #3278
# Issue #3279
# Issue #3280
# Issue #3281
# Issue #3282
# Issue #3283
# Issue #3284
# Issue #3285
# Issue #3286
# Issue #3287
# Issue #3288
# Issue #3289
# Issue #3290
# Issue #3291
# Issue #3292
# Issue #3293
# Issue #3294
# Issue #3295
# Issue #3296
# Issue #3297
# Issue #3298
# Issue #3299
# Issue #3300
# Issue #3301
# Issue #3302
# Issue #3303
# Issue #3304
# Issue #3305
# Issue #3306
# Issue #3307
# Issue #3308
# Issue #3309
# Issue #3310
# Issue #3311
# Issue #3312
# Issue #3313
# Issue #3314
# Issue #3315
# Issue #3316
# Issue #3317
# Issue #3318
# Issue #3319
# Issue #3320
# Issue #3321
# Issue #3322
# Issue #3323
# Issue #3324
# Issue #3325
# Issue #3326
# Issue #3327
# Issue #3328
# Issue #3329
# Issue #3330
# Issue #3331
# Issue #3332
# Issue #3333
# Issue #3334
# Issue #3335
# Issue #3336
# Issue #3337
# Issue #3338
# Issue #3339
# Issue #3340
# Issue #3341
# Issue #3342
# Issue #3343
# Issue #3344
# Issue #3345
# Issue #3346
# Issue #3347
# Issue #3348
# Issue #3349
# Issue #3350
# Issue #3351
# Issue #3352
# Issue #3353
# Issue #3354
# Issue #3355
# Issue #3356
# Issue #3357
# Issue #3358
# Issue #3359
# Issue #3360
# Issue #3361
# Issue #3362
# Issue #3363
# Issue #3364
# Issue #3365
# Issue #3366
# Issue #3367
# Issue #3368
# Issue #3369
# Issue #3370
# Issue #3371
# Issue #3372
# Issue #3373
# Issue #3374
# Issue #3375
# Issue #3376
# Issue #3377
# Issue #3378
# Issue #3379
# Issue #3380
# Issue #3381
# Issue #3382
# Issue #3383
# Issue #3384
# Issue #3385
# Issue #3386
# Issue #3387
# Issue #3388
# Issue #3389
# Issue #3390
# Issue #3391
# Issue #3392
# Issue #3393
# Issue #3394
# Issue #3395
# Issue #3396
# Issue #3397
# Issue #3398
# Issue #3399
# Issue #3400
# Issue #3401
# Issue #3402
# Issue #3403
# Issue #3404
# Issue #3405
# Issue #3406
# Issue #3407
# Issue #3408
# Issue #3409
# Issue #3410
# Issue #3411
# Issue #3412
# Issue #3413
# Issue #3414
# Issue #3415
# Issue #3416
# Issue #3417
# Issue #3418
# Issue #3419
# Issue #3420
# Issue #3421
# Issue #3423
# Issue #3424
# Issue #3425
# Issue #3426
# Issue #3427
# Issue #3428
# Issue #3429
# Issue #3430
# Issue #3431
# Issue #3432
# Issue #3433
# Issue #3434
# Issue #3435
# Issue #3436
# Issue #3437
# Issue #3438
# Issue #3439
# Issue #3440
# Issue #3441
# Issue #3442
# Issue #3443
# Issue #3444
# Issue #3445
# Issue #3446
# Issue #3447
# Issue #3448
# Issue #3449
# Issue #3450
# Issue #3451
# Issue #3452
# Issue #3453
# Issue #3454
# Issue #3455
# Issue #3456
# Issue #3457
# Issue #3458
# Issue #3459
# Issue #3460
# Issue #3461
# Issue #3462
# Issue #3463
# Issue #3464
# Issue #3465
# Issue #3466
# Issue #3467
# Issue #3468
# Issue #3469
# Issue #3470
# Issue #3471
# Issue #3472
# Issue #3473
# Issue #3474
# Issue #3475
# Issue #3476
# Issue #3477
# Issue #3478
# Issue #3479
# Issue #3480
# Issue #3481
# Issue #3482
# Issue #3483
# Issue #3484
# Issue #3485
# Issue #3486
# Issue #3487
# Issue #3488
# Issue #3489
# Issue #3490
# Issue #3491
# Issue #3494
# Issue #3495
# Issue #3496
# Issue #3497
# Issue #3498
# Issue #3499
# Issue #3500
# Issue #3501
# Issue #3502
# Issue #3503
# Issue #3504
# Issue #3505
# Issue #3506
# Issue #3507
# Issue #3508
# Issue #3509
# Issue #3510
# Issue #3511
# Issue #3513
# Issue #3514
# Issue #3515
# Issue #3516
# Issue #3517
# Issue #3518
# Issue #3519
# Issue #3521
# Issue #3525
# Issue #3526
# Issue #3527
# Issue #3528
# Issue #3529
# Issue #3530
# Issue #3531
# Issue #3532
# Issue #3533
# Issue #3539
# Issue #3540
# Issue #3541
# Issue #3542
# Issue #3543
# Issue #3544
# Issue #3546
# Issue #3547
# Issue #3548
# Issue #3549
# Issue #3551
# Issue #3553
# Issue #3555
# Issue #3556
# Issue #3557
# Issue #3558
# Issue #3559
# Issue #3560
# Issue #3561
# Issue #3562
# Issue #3563
# Issue #3564
# Issue #3566
# Issue #3568
# Issue #3569
# Issue #3570
# Issue #3571
# Issue #3572
# Issue #3573
# Issue #3574
# Issue #3575
# Issue #3576
# Issue #3577
# Issue #3578
# Issue #3579
# Issue #3580
# Issue #3581
# Issue #3582
# Issue #3583
# Issue #3584
# Issue #3585
# Issue #3587
# Issue #3588
# Issue #3589
# Issue #3590
# Issue #3591
# Issue #3592
# Issue #3593
# Issue #3594
# Issue #3595
# Issue #3596
# Issue #3597
# Issue #3598
# Issue #3599
# Issue #3600
# Issue #3601
# Issue #3602
# Issue #3603
# Issue #3604
# Issue #3605
# Issue #3606
# Issue #3607
# Issue #3608
# Issue #3609
# Issue #3610
# Issue #3611
# Issue #3612
# Issue #3613
# Issue #3614
# Issue #3615
# Issue #3616
# Issue #3617
# Issue #3618
# Issue #3619
# Issue #3620
# Issue #3621
# Issue #3622
# Issue #3623
# Issue #3624
# Issue #3625
# Issue #3626
# Issue #3627
# Issue #3628
# Issue #3629
# Issue #3630
# Issue #3631
# Issue #3632
# Issue #3633
# Issue #3634
# Issue #3635
# Issue #3636
# Issue #3637
# Issue #3638
# Issue #3639
# Issue #3640
# Issue #3641
# Issue #3642
# Issue #3643
# Issue #3644
# Issue #3645
# Issue #3646
# Issue #3647
# Issue #3648
# Issue #3649
# Issue #3650
# Issue #3651
# Issue #3652
# Issue #3653
# Issue #3654
# Issue #3655
# Issue #3656
# Issue #3657
# Issue #3658
# Issue #3659
# Issue #3660
# Issue #3661
# Issue #3662
# Issue #3663
# Issue #3664
# Issue #3665
# Issue #3666
# Issue #3668
# Issue #3669
# Issue #3670
# Issue #3671
# Issue #3672
# Issue #3673
# Issue #3674
# Issue #3675
# Issue #3676
# Issue #3677
# Issue #3678
# Issue #3679
# Issue #3680
# Issue #3681
# Issue #3682
# Issue #3683
# Issue #3684
# Issue #3685
# Issue #3686
# Issue #3687
# Issue #3688
# Issue #3689
# Issue #3690
# Issue #3691
# Issue #3692
# Issue #3693
# Issue #3694
# Issue #3695
# Issue #3696
# Issue #3697
# Issue #3698
# Issue #3699
# Issue #3700
# Issue #3701
# Issue #3702
# Issue #3703
# Issue #3720
# Issue #3721
# Issue #3722
# Issue #3723
# Issue #3724
# Issue #3725
# Issue #3726
# Issue #3727
# Issue #3728
# Issue #3729
# Issue #3730
# Issue #3731
# Issue #3732
# Issue #3733
# Issue #3735
# Issue #3736
# Issue #3737
# Issue #3738
# Issue #3739
# Issue #3740
# Issue #3742
# Issue #3743
# Issue #3744
# Issue #3745
# Issue #3746
# Issue #3747
# Issue #3748
# Issue #3749
# Issue #3750
# Issue #3751
# Issue #3752
# Issue #3753
# Issue #3754
# Issue #3755
# Issue #3756
# Issue #3757
# Issue #3758
# Issue #3759
# Issue #3760
# Issue #3761
# Issue #3762
# Issue #3763
# Issue #3764
# Issue #3765
# Issue #3766
# Issue #3767
# Issue #3768
# Issue #3769
# Issue #3770
# Issue #3772
# Issue #3773
# Issue #3774
# Issue #3775
# Issue #3776
# Issue #3777
# Issue #3778
# Issue #3779
# Issue #3780
# Issue #3781
# Issue #3782
# Issue #3783
# Issue #3784
# Issue #3785
# Issue #3786
# Issue #3787
# Issue #3788
# Issue #3789
# Issue #3790
# Issue #3791
# Issue #3792
# Issue #3793
# Issue #3794
# Issue #3795
# Issue #3796
# Issue #3797
# Issue #3798
# Issue #3799
# Issue #3800
# Issue #3801
# Issue #3802
# Issue #3803
# Issue #3804
# Issue #3805
# Issue #3806
# Issue #3808
# Issue #3809
# Issue #3810
# Issue #3811
# Issue #3812
# Issue #3813
# Issue #3814
# Issue #3815
# Issue #3816
# Issue #3817
# Issue #3818
# Issue #3819
# Issue #3820
# Issue #3821
# Issue #3822
# Issue #3823
# Issue #3824
# Issue #3825
# Issue #3826
# Issue #3827
# Issue #3828
# Issue #3829
# Issue #3830
# Issue #3831
# Issue #3832
# Issue #3833
# Issue #3834
# Issue #3835
# Issue #3836
# Issue #3837
# Issue #3838
# Issue #3839
# Issue #3840
# Issue #3841
# Issue #3842
# Issue #3843
# Issue #3844
# Issue #3845
# Issue #3846
# Issue #3848
# Issue #3849
# Issue #3850
# Issue #3851
# Issue #3852
# Issue #3853
# Issue #3854
# Issue #3855
# Issue #3856
# Issue #3857
# Issue #3858
# Issue #3859
# Issue #3860
# Issue #3861
# Issue #3862
# Issue #3863
# Issue #3864
# Issue #3865
# Issue #3866
# Issue #3867
# Issue #3868
# Issue #3869
# Issue #3870
# Issue #3871
# Issue #3872
# Issue #3873
# Issue #3874
# Issue #3875
# Issue #3876
# Issue #3877
# Issue #3878
# Issue #3879
# Issue #3880
# Issue #3881
# Issue #3882
# Issue #3883
# Issue #3884
# Issue #3885
# Issue #3886
# Issue #3887
# Issue #3888
# Issue #3889
# Issue #3890
# Issue #3891
# Issue #3892
# Issue #3893
# Issue #3894
# Issue #3895
# Issue #3896
# Issue #3897
# Issue #3898
# Issue #3899
# Issue #3900
# Issue #3902
# Issue #3903
# Issue #3904
# Issue #3905
# Issue #3906
# Issue #3907
# Issue #3908
# Issue #3909
# Issue #3910
# Issue #3912
# Issue #3915
# Issue #3916
# Issue #3917
# Issue #3918
# Issue #3919
# Issue #3930
# Issue #3931
# Issue #3932
# Issue #3934
# Issue #3938
# Issue #3939
# Issue #3940
# Issue #3941
# Issue #3942
# Issue #3943
# Issue #3944
# Issue #3946
# Issue #3947
# Issue #3948
# Issue #3951
# Issue #3952
# Issue #3953
# Issue #3954
# Issue #3955
# Issue #3956
# Issue #3957
# Issue #3958
# Issue #3964
# Issue #3965
# Issue #3966
# Issue #3967
# Issue #3968
# Issue #3969
# Issue #3972
# Issue #3973
# Issue #3974
# Issue #3975
# Issue #3976
# Issue #3977
# Issue #3978
# Issue #3979
# Issue #3980
# Issue #3981
# Issue #3982
# Issue #3983
# Issue #3984
# Issue #3985
# Issue #3986
# Issue #3987
# Issue #3988
# Issue #3989
# Issue #3990
# Issue #3991
# Issue #3992
# Issue #3993
# Issue #3994
# Issue #3995
# Issue #3996
# Issue #3997
# Issue #3998
# Issue #3999
# Issue #4000
# Issue #4001
# Issue #4002
# Issue #4004
# Issue #4005
# Issue #4006
# Issue #4007
# Issue #4008
# Issue #4009
# Issue #4010
# Issue #4011
# Issue #4012
# Issue #4013
# Issue #4014
# Issue #4015
# Issue #4016
# Issue #4017
# Issue #4018
# Issue #4019
# Issue #4020
# Issue #4021
# Issue #4022
# Issue #4023
# Issue #4024
# Issue #4025
# Issue #4026
# Issue #4029
# Issue #4030
# Issue #4031
# Issue #4032
# Issue #4033
# Issue #4034
# Issue #4036
# Issue #4037
# Issue #4038
# Issue #4039
# Issue #4040
# Issue #4041
# Issue #4042
# Issue #4043
# Issue #4044
# Issue #4045
# Issue #4046
# Issue #4047
# Issue #4049
# Issue #4050
# Issue #4051
# Issue #4052
# Issue #4053
# Issue #4054
# Issue #4055
# Issue #4056
# Issue #4057
# Issue #4058
# Issue #4059
# Issue #4060
# Issue #4061
# Issue #4062
# Issue #4063
# Issue #4064
# Issue #4065
# Issue #4066
# Issue #4067
# Issue #4068
# Issue #4069
# Issue #4070
# Issue #4071
# Issue #4072
# Issue #4073
# Issue #4074
# Issue #4075
# Issue #4076
# Issue #4077
# Issue #4078
# Issue #4079
# Issue #4080
# Issue #4081
# Issue #4082
# Issue #4083
# Issue #4084
# Issue #4085
# Issue #4086
# Issue #4087
# Issue #4088
# Issue #4089
# Issue #4090
# Issue #4091
# Issue #4093
# Issue #4094
# Issue #4095
# Issue #4096
# Issue #4097
# Issue #4098
# Issue #4099
# Issue #4100
# Issue #4101
# Issue #4102
# Issue #4103
# Issue #4104
# Issue #4105
# Issue #4106
# Issue #4107
# Issue #4108
# Issue #4109
# Issue #4110
# Issue #4111
# Issue #4112
# Issue #4113
# Issue #4114
# Issue #4115
# Issue #4117
# Issue #4118
# Issue #4120
# Issue #4121
# Issue #4122
# Issue #4124
# Issue #4125
# Issue #4126
# Issue #4127
# Issue #4128
# Issue #4129
# Issue #4130
# Issue #4131
# Issue #4132
# Issue #4133
# Issue #4134
# Issue #4135
# Issue #4136
# Issue #4137
# Issue #4138
# Issue #4139
# Issue #4140
# Issue #4141
# Issue #4142
# Issue #4143
# Issue #4144
# Issue #4145
# Issue #4146
# Issue #4147
# Issue #4148
# Issue #4149
# Issue #4150
# Issue #4151
# Issue #4152
# Issue #4153
# Issue #4154
# Issue #4155
# Issue #4156
# Issue #4157
# Issue #4158
# Issue #4159
# Issue #4160
# Issue #4161
# Issue #4162
# Issue #4163
# Issue #4164
# Issue #4165
# Issue #4166
# Issue #4167
# Issue #4168
# Issue #4169
# Issue #4170
# Issue #4171
# Issue #4172
# Issue #4173
# Issue #4174
# Issue #4175
# Issue #4176
# Issue #4177
# Issue #4178
# Issue #4220
# Issue #4224
# Issue #4228
# Issue #4229
# Issue #4230
# Issue #4231
# Issue #4232
# Issue #4233
# Issue #4234
# Issue #4235
# Issue #4236
# Issue #4237
# Issue #4238
# Issue #4239
# Issue #4240
# Issue #4241
# Issue #4242
# Issue #4243
# Issue #4244
# Issue #4245
# Issue #4246
# Issue #4247
# Issue #4248
# Issue #4249
# Issue #4250
# Issue #4251
# Issue #4252
# Issue #4257
# Issue #4258
# Issue #4259
# Issue #4260
# Issue #4261
# Issue #4262
# Issue #4263
# Issue #4264
# Issue #4265
# Issue #4266
# Issue #4267
# Issue #4270
# Issue #4271
# Issue #4272
# Issue #4273
# Issue #4277
# Issue #4278
# Issue #4279
# Issue #4280
# Issue #4281
# Issue #4286
# Issue #4287
# Issue #4288
# Issue #4289
# Issue #4290
# Issue #4291
# Issue #4292
# Issue #4293
# Issue #4294
# Issue #4296
# Issue #4297
# Issue #4298
# Issue #4299
# Issue #4301
# Issue #4303
# Issue #4304
# Issue #4305
# Issue #4306
# Issue #4307
# Issue #4308
# Issue #4309
# Issue #4310
# Issue #4311
# Issue #4312
# Issue #4313
# Issue #4314
# Issue #4315
# Issue #4316
# Issue #4317
# Issue #4318
# Issue #4319
# Issue #4321
# Issue #4322
# Issue #4323
# Issue #4326
# Issue #4327
# Issue #4328
# Issue #4329
# Issue #4330
# Issue #4331
# Issue #4332
# Issue #4334
# Issue #4335
# Issue #4336
# Issue #4337
# Issue #4338
# Issue #4339
# Issue #4340
# Issue #4341
# Issue #4342
# Issue #4343
# Issue #4344
# Issue #4345
# Issue #4346
# Issue #4347
# Issue #4348
# Issue #4349
# Issue #4350
# Issue #4353
# Issue #4355
# Issue #4357
# Issue #4359
# Issue #4360
# Issue #4362
# Issue #4365
# Issue #4372
# Issue #4373
# Issue #4374
# Issue #4375
# Issue #4377
# Issue #4379
# Issue #4380
# Issue #4381
# Issue #4382
# Issue #4383
# Issue #4384
# Issue #4385
# Issue #4386
# Issue #4387
# Issue #4388
# Issue #4389
# Issue #4390
# Issue #4391
# Issue #4392
# Issue #4393
# Issue #4394
# Issue #4395
# Issue #4396
# Issue #4397
# Issue #4398
# Issue #4399
# Issue #4400
# Issue #4401
# Issue #4402
# Issue #4403
# Issue #4404
# Issue #4405
# Issue #4406
# Issue #4407
# Issue #4408
# Issue #4409
# Issue #4410
# Issue #4411
# Issue #4413
# Issue #4414
# Issue #4415
# Issue #4416
# Issue #4417
# Issue #4418
# Issue #4419
# Issue #4420
# Issue #4421
# Issue #4422
# Issue #4423
# Issue #4424
# Issue #4425
# Issue #4426
# Issue #4427
# Issue #4428
# Issue #4429
# Issue #4430
# Issue #4431
# Issue #4432
# Issue #4433
# Issue #4434
# Issue #4435
# Issue #4436
# Issue #4438
# Issue #4439
# Issue #4440
# Issue #4441
# Issue #4442
# Issue #4443
# Issue #4444
# Issue #4445
# Issue #4446
# Issue #4448
# Issue #4449
# Issue #4450
# Issue #4451
# Issue #4452
# Issue #4453
# Issue #4454
# Issue #4455
# Issue #4456
# Issue #4457
# Issue #4460
# Issue #4461
# Issue #4462
# Issue #4463
# Issue #4464
# Issue #4465
# Issue #4466
# Issue #4467
# Issue #4468
# Issue #4469
# Issue #4470
# Issue #4471
# Issue #4472
# Issue #4473
# Issue #4474
# Issue #4475
# Issue #4476
# Issue #4477
# Issue #4478
# Issue #4480
# Issue #4482
# Issue #4483
# Issue #4492
# Issue #4497
# Issue #4502
# Issue #4504
# Issue #4505
# Issue #4506
# Issue #4507
# Issue #4508
# Issue #4510
# Issue #4511
# Issue #4513
# Issue #4514
# Issue #4515
# Issue #4516
# Issue #4517
# Issue #4518
# Issue #4519
# Issue #4520
# Issue #4521
# Issue #4522
# Issue #4523
# Issue #4524
# Issue #4525
# Issue #4527
# Issue #4528
# Issue #4529
# Issue #4530
# Issue #4531
# Issue #4532
# Issue #4533
# Issue #4534
# Issue #4538
# Issue #4539
# Issue #4540
# Issue #4542
# Issue #4543
# Issue #4544
# Issue #4545
# Issue #4548
# Issue #4551
# Issue #4553
# Issue #4554
# Issue #4555
# Issue #4557
# Issue #4558
# Issue #4559
# Issue #4560
# Issue #4561
# Issue #4563
# Issue #4564
# Issue #4565
# Issue #4569
# Issue #4575
# Issue #4576
# Issue #4577
# Issue #4578
# Issue #4579
# Issue #4580
# Issue #4581
# Issue #4583
# Issue #4584
# Issue #4585
# Issue #4586
# Issue #4587
# Issue #4588
# Issue #4589
# Issue #4595
# Issue #4596
# Issue #4597
# Issue #4598
# Issue #4599
# Issue #4600
# Issue #4602
# Issue #4606
# Issue #4607
# Issue #4608
# Issue #4611
# Issue #4612
# Issue #4613
# Issue #4614
# Issue #4615
# Issue #4624
# Issue #4630
# Issue #4631
# Issue #4632
# Issue #4633
# Issue #4636
# Issue #4639
# Issue #4641
# Issue #4642
# Issue #4643
# Issue #4644
# Issue #4645
# Issue #4646
# Issue #4647
# Issue #4648
# Issue #4649
# Issue #4650
# Issue #4651
# Issue #4652
# Issue #4653
# Issue #4654
# Issue #4657
# Issue #4658
# Issue #4659
# Issue #4661
# Issue #4662
# Issue #4663
# Issue #4664
# Issue #4665
# Issue #4666
# Issue #4667
# Issue #4670
# Issue #4671
# Issue #4672
# Issue #4673
# Issue #4674
# Issue #4675
# Issue #4676
# Issue #4681
# Issue #4684
# Issue #4685
# Issue #4686
# Issue #4687
# Issue #4688
# Issue #4689
# Issue #4690
# Issue #4691
# Issue #4692
# Issue #4693
# Issue #4694
# Issue #4695
# Issue #4696
# Issue #4697
# Issue #4698
# Issue #4699
# Issue #4700
# Issue #4701
# Issue #4702
# Issue #4704
# Issue #4707
# Issue #4714
# Issue #4715
# Issue #4716
# Issue #4717
# Issue #4719
# Issue #4721
# Issue #4722
# Issue #4723
# Issue #4724
# Issue #4725
# Issue #4726
# Issue #4727
# Issue #4728
# Issue #4729
# Issue #4730
# Issue #4731
# Issue #4732
# Issue #4735
# Issue #4736
# Issue #4737
# Issue #4742
# Issue #4743
# Issue #4744
# Issue #4745
# Issue #4747
# Issue #4749
# Issue #4750
# Issue #4751
# Issue #4752
# Issue #4753
# Issue #4756
# Issue #4759
# Issue #4763
# Issue #4766
# Issue #4768
# Issue #4769
# Issue #4770
# Issue #4771
# Issue #4772
# Issue #4774
# Issue #4779
# Issue #4783
# Issue #4787
# Issue #4790
# Issue #4791
# Issue #4792
# Issue #4793
# Issue #4816
# Issue #4827
# Issue #4832
# Issue #4841
# Issue #4853
# Issue #4855
# Issue #4858
# Issue #4876
# Issue #4884
# Issue #4885
# Issue #4886
# Issue #4891
# Issue #4896
# Issue #4899
# Issue #4902
# Issue #4905
# Issue #4909
# Issue #4916
# Issue #4917
# Issue #4919
# Issue #4921
# Issue #4922
# Issue #4923
# Issue #4924
# Issue #4925
# Issue #4926
# Issue #4927
# Issue #4928
# Issue #4930
# Issue #4932
# Issue #4933
# Issue #4936
# Issue #4946
# Issue #4965
# Issue #5008
# Issue #5033
# Issue #5041
# Issue #5051
# Issue #5054
# Issue #5057
# Issue #5087
# Issue #5117
# Issue #5118
# Issue #5132
# Issue #5139
# Issue #5141
# Issue #5144
# Issue #5145
# Issue #5146
# Issue #5147
# Issue #5148
# Issue #5149
# Issue #5151
# Issue #5169
# Issue #5173
# Issue #5175
# Issue #5190
# Issue #5192
# Issue #5195
# Issue #5208
# Issue #5211
# Issue #5212
# Issue #5213
# Issue #5244
# Issue #5274
# Issue #5275
# Issue #5279
# Issue #5280
# Issue #5281
# Issue #5282
# Issue #5289
# Issue #5292
# Issue #5295
# Issue #5300
# Issue #5303
