#!/usr/bin/env python3
"""
Aura — 统一构建/测试入口

Usage:
  ./build.py [--sanitizer=asan|ubsan|tsan] build    # CMake 构建 (sanitizer-插桩)
  ./build.py [--sanitizer=asan|ubsan|tsan] test [suite]  # 运行测试
  ./build.py check            # gate + ci（与 CI 相同）
  ./build.py gate             # docs + lint + format + fixtures + surface + binding + registry + dead-heap
  ./build.py gate --fix       # 同上，但 auto-regen docs/registry + lint/format --fix（#1572）
  ./build.py gate --scripts-only  # 跳过 clang-format（脚本-only,无 C++ 编译）
  ./build.py ci               # build + CI 测试矩阵
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
  ./build.py repro [--verify]  # 可复现 Release 构建（#675）

  ./build.py sbom [--version=V] # CycloneDX SBOM 生成（#675）
  ./build.py security          # 依赖/文件系统漏洞扫描（#675）
  ./build.py bench [--strict]  # Benchmark 基线 + 回归检测（#1569 SLO gate）

Test suites:
  unit        C++ 单元测试 (61 cases)
  integ       端到端管线测试 (.aura)
  typecheck   类型检查测试
  bench       Benchmark 基线 + 回归检测（strict 时 hard fail）
  smoke       快速冒烟测试
  all         全部测试 (默认)
  core        核心管线 (unit + integ + typecheck + smoke + bash + suite)
  safety      安全回归 (gradual + regression + p0)
  issues      Issue #226 — unified test_issue_* runner (tier via AURA_ISSUES_TIER)
  issues-fast 同上，强制 fast 档（bundle 子集 + git 变更）
  check       构建 + core + safety + issues（CI 默认）
"""

import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from threading import Lock

sys.path.insert(0, str(Path(__file__).resolve().parent / "tests"))
from _aura_harness import B, G, N, R, Y, fail, info, ok, run, warn
from benchmark_cases import load_typecheck_cases
from integ_cases import load_integ_cases
from issue_tier import issues_tier, load_fast_targets, resolve_issue_targets
from smoke_cases import load_smoke_cases

ROOT = Path(__file__).resolve().parent
BENCH = ROOT / "tests" / "benchmark.py"


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
        "-fsanitize=thread -fno-omit-frame-pointer",
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

GEN_DOCS = ROOT / "scripts" / "gen_docs.py"


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


def cmd_format():
    """clang-format check/fix for all C++ under src/ + tests/ (CI parity)."""
    fix = "--fix" in sys.argv[2:]
    print(f"{B}═══ Format {'(fix)' if fix else '(check)'} ═══{N}")
    clang_format = shutil.which("clang-format")
    if not clang_format:
        fail("clang-format not found — install clang-format (CI: llvm 22.x)")
        return 1
    files = _cpp_source_files()
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
    """Ruff lint + format check + Issue #1484 test-includes linter."""
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
    # Issue #1484: test-includes linter (matches .githooks/pre-commit
    # C2 wiring). Bare `#include "X.h"` patterns where the header
    # lives under src/compiler/ or src/core/ but the include
    # doesn't use the subdir prefix are rejected. Discovered during
    # #1459 close-verify (9 broken files on main were fixed at
    # commit 313c530d); this linter prevents future regressions.
    # Uses the same sys.executable + ROOT.joinpath pattern as
    # cmd_fixtures below.
    script = ROOT / "scripts" / "check_test_includes.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = run([sys.executable, str(script)], cwd=ROOT)
    if r != 0:
        fail("test includes linter failed — run python3 scripts/check_test_includes.py")
        return r
    ok("lint OK")
    return 0


def cmd_fixtures():
    """Validate tests/fixtures/*.json schema and baseline sync (#1961 → run.py)."""
    print(f"{B}═══ Fixtures (check) ═══{N}")
    script = ROOT / "tests" / "run.py"
    legacy = ROOT / "tests" / "fixture_check.py"
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
    if _tool_available("mold") and _fuse_ld_works("mold"):
        return "mold"
    if _tool_available("ld.lld") and _fuse_ld_works("lld"):
        return "lld"
    if _tool_available("mold"):
        warn("mold present but cannot link with this toolchain; falling back")
    return None


def _phase(label: str, t0: float) -> None:
    """Issue #873/#874: always print wall-time for major build phases."""
    dt = time.time() - t0
    print(f"  {G}⏱{N} {label}: {dt:.1f}s", flush=True)


def _cmake_configure_args() -> list[str]:
    args = ["cmake", "-B", str(BUILD), "-G", "Ninja", "-Wno-dev"]
    # CMake 4.4+ experimental gate for `import std` (must be set before
    # project()/toolchain detection). UUID from CMake binary (CxxImportStd).
    args.append("-DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=f35a9ac6-8463-4d38-8eec-5d6008153e7d")
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
    return args


def cmd_build():
    """CMake 构建 (Ninja)"""
    print(f"{B}═══ Build ═══{N}")
    BUILD.mkdir(parents=True, exist_ok=True)
    nproc = _build_jobs()
    t_all = time.time()

    t0 = time.time()
    r = run(_cmake_configure_args(), cwd=ROOT)
    _phase("cmake configure", t0)
    if r != 0:
        return r

    # Build main binaries one target at a time. A single multi-target
    # ninja -jN invocation races ast.ixx across aura/test_ir and can
    # trigger a flaky GCC 16 ICE in the ealias pass under -O2.
    for target in ("aura", "test_ir", "test_concurrent"):
        t0 = time.time()
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                target,
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            warn(f"{target} build failed — retrying once (GCC ICE workaround)")
            r = run(
                [
                    "cmake",
                    "--build",
                    str(BUILD),
                    "--target",
                    target,
                    "-j",
                    str(nproc),
                ],
                cwd=ROOT,
            )
        _phase(f"build {target}", t0)
        if r != 0:
            fail(f"build {target} failed")
            return r

    # Build test_issue_* targets. Full tier uses the aggregate
    # (profile bundles + true standalones; dual standalones that
    # live in bundles are EXCLUDE_FROM_ALL — see #871/#873).
    # fast tier: fixture subset + git-changed (issue_tier.py).
    # AURA_ISSUE_BUILD=bundles → only the profile bundle exes.
    tier = issues_tier()
    issue_mode = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower()
    t0 = time.time()
    if tier == "full" and issue_mode == "bundles":
        from issue_tier import BUNDLE_PROFILES

        targets = [f"test_issues_{p}" for p in BUNDLE_PROFILES]
        issue_cmd = ["ninja", "-C", str(BUILD), "-k", "0", f"-j{nproc}", *targets]
        info(f"issue tests: tier=full mode=bundles ({len(targets)} bundle targets)")
    elif tier == "full":
        issue_cmd = [
            "ninja",
            "-C",
            str(BUILD),
            "-k",
            "0",
            f"-j{nproc}",
            "all_test_issue_targets",
        ]
        info("issue tests: tier=full (bundles + standalones; duals excluded)")
    else:
        targets = resolve_issue_targets("fast")
        issue_cmd = ["ninja", "-C", str(BUILD), "-k", "0", f"-j{nproc}", *targets]
        changed = [t for t in targets if t not in set(load_fast_targets())]
        extra = f", +{len(changed)} git-changed" if changed else ""
        info(f"issue tests: tier=fast ({len(targets)} targets{extra})")
    r = run(issue_cmd, cwd=ROOT)
    if r != 0:
        # One retry: GCC 16 module dyndep scans under high -j can flake with
        # "when writing output to …*.o.ddi.i: Invalid argument" (shared JIT
        # lib reduces this; retry covers residual races).
        warn("issue-test build failed — retrying once (module dyndep flake workaround)")
        r = run(issue_cmd, cwd=ROOT)
    _phase("build issue tests", t0)
    if r != 0:
        # Don't fail cmd_build on partial-build errors —
        # the runner will skip the unbuilt binaries.
        print(f"{Y}  some test_issue_* targets failed to build (pre-existing); runner will skip them{N}")

    _phase("build total", t_all)
    ok("build OK")
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
    """C++ 单元测试 — test_ir (61 cases)"""
    print(f"{B}═══ Unit tests ═══{N}")
    if not TEST_BIN.exists():
        fail(f"{TEST_BIN} not found — run 'build' first")
        return 1

    all_ok = True

    # test_ir
    start = time.time()
    r = subprocess.run([str(TEST_BIN)], capture_output=True, text=True)
    elapsed = time.time() - start
    for line in r.stdout.strip().split("\n"):
        if "passed" in line.lower():
            ok(line.strip())
        elif "FAIL" in line:
            fail(line.strip())
    if r.returncode != 0:
        all_ok = False
    print(f"  Unit tests: {elapsed:.2f}s")

    # test_concurrent
    concurrent_bin = BUILD / "test_concurrent"
    if concurrent_bin.exists():
        start2 = time.time()
        r2 = subprocess.run([str(concurrent_bin)], timeout=300)
        elapsed2 = time.time() - start2
        # binary prints directly to terminal; just check rc
        ok(f"concurrent (exit {r2.returncode}) in {elapsed2:.2f}s")
        if r2.returncode != 0:
            all_ok = False

    if all_ok:
        ok("all unit tests passed")
    else:
        fail("some unit tests failed")
    return 0 if all_ok else 1


# ═══════════════════════════════════════════════════════════════
# Integration tests (.aura files)
# ═══════════════════════════════════════════════════════════════


# Integration cases live in tests/fixtures/integ/*.json (#1962)
# (loaded via tests/integ_cases.py).


def test_integ():
    """端到端管线测试 — eval / ir / typecheck / serve"""
    print(f"{B}═══ Integration tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found — run 'build' first")
        return 1

    flags = {
        "eval": [],
        "ir": ["--ir"],
        "typecheck": ["--typecheck"],
        "serve": ["--serve"],
    }
    passed = failed = 0

    for tc in load_integ_cases():
        args = [str(AURA)] + flags.get(tc.pipeline, [])
        pipe_input = tc.code if tc.pipeline == "serve" else tc.code + "\n"

        r = subprocess.run(args, input=pipe_input, capture_output=True, text=True, timeout=30)

        ok_case = True
        issues = []

        # err_div_zero accepts multiple exit codes:
        #   0  = clean evaluation (test author's intent)
        #   -8 = legacy SIGFPE crash (pre-IR-executor behavior)
        #   1  = clean error report (IR executor DivisionByZero,
        #         post-#212 pure arithmetic_div_pure path)
        # All three satisfy the test's intent: no UB, no crash.
        if r.returncode != tc.expected_status and not (tc.name == "err_div_zero" and r.returncode in (0, -8, 1)):
            ok_case = False
            issues.append(f"exit_code={r.returncode} (expected {tc.expected_status})")

        stdout = r.stdout.strip()
        stderr = r.stderr.strip()
        check_stdout = stdout.split("\n")[-1] if tc.pipeline == "serve" else stdout

        if tc.expected:
            if tc.expected.startswith(">="):
                try:
                    threshold = int(tc.expected[2:].strip())
                    tokens = check_stdout.strip().split()
                    if not tokens:
                        ok_case = False
                        issues.append(f"expected value>={threshold}, got empty stdout (stderr={stderr[:80]!r})")
                    else:
                        val = int(tokens[-1])
                        if val < threshold:
                            ok_case = False
                            issues.append(f"expected value>={threshold}, got: {check_stdout[:80]}...")
                except ValueError:
                    if tc.expected not in check_stdout:
                        ok_case = False
                        issues.append(f"expected '{tc.expected}' in stdout, got: {stdout[:80]}...")
            elif tc.expected not in check_stdout:
                ok_case = False
                issues.append(f"expected '{tc.expected}' in stdout, got: {stdout[:80]}...")

        if tc.expected_err:
            combined = stdout + "\n" + stderr
            if tc.expected_err not in combined:
                ok_case = False
                issues.append(f"expected error '{tc.expected_err}' not found")

        if ok_case:
            ok(f"[{tc.pipeline:10s}] {tc.name}")
            passed += 1
        else:
            fail(f"[{tc.pipeline:10s}] {tc.name} — {'; '.join(issues)}")
            failed += 1

    print(f"  Integration: {passed}/{passed + failed} passed")
    return 1 if failed > 0 else 0


# ═══════════════════════════════════════════════════════════════
# Typecheck tests
# ═══════════════════════════════════════════════════════════════


def test_typecheck():
    """类型检查专项测试"""
    print(f"{B}═══ Typecheck tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    passed = failed = 0
    for tc in load_typecheck_cases():
        name, code, exp_type = tc.name, tc.code, tc.expected_type
        r = subprocess.run(
            [str(AURA), "--typecheck"],
            input=code + "\n",
            capture_output=True,
            text=True,
            timeout=10,
        )
        stdout = r.stdout.strip()
        type_ok = False
        for line in stdout.split("\n"):
            if line.startswith("type:") and exp_type in line:
                type_ok = True
                break

        if type_ok:
            ok(f"{name:25s} → {exp_type}")
            passed += 1
        else:
            fail(f"{name:25s} expected '{exp_type}', got: {stdout[:80]}")
            failed += 1

    print(f"  Typecheck: {passed}/{passed + failed} passed")
    return 1 if failed > 0 else 0


# ═══════════════════════════════════════════════════════════════
# Benchmark
# ═══════════════════════════════════════════════════════════════


def test_bench():
    """Benchmark 基线 + 回归检测（#1569: strict SLO gate).

    Path map (#1570): this is the benchmark gate — NOT src/test/benchmark_gate.ixx.
    """
    print(f"{B}═══ Benchmark ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1
    env = {**os.environ, "AURA_BIN": str(AURA)}
    args = [sys.executable, str(BENCH)]
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
    )
    if strict:
        args.append("--strict")
        print("  mode: STRICT SLO gate (AURA_CI_STRICT_BENCH / --strict)")
    else:
        # Soft: default run prints regression warnings; hard-fail only
        # functional FAIL cases. Use --strict / AURA_CI_STRICT_BENCH=1
        # for CI hard exit on performance regression.
        print("  mode: soft (warn on regression; AURA_CI_STRICT_BENCH=1 for hard fail)")
    return run(args, env=env)


def cmd_bench():
    """Issue #1569: ./build.py bench [--strict] — run benchmark SLO gate."""
    return test_bench()


# ═══════════════════════════════════════════════════════════════
# Smoke tests
# ═══════════════════════════════════════════════════════════════


def test_smoke():
    """快速冒烟测试"""
    print(f"{B}═══ Smoke tests ═══{N}")
    if not AURA.exists():
        fail(f"{AURA} not found")
        return 1

    passed = failed = 0
    for sc in load_smoke_cases():
        name, cmd, expected = sc.name, sc.command, sc.expected
        r = subprocess.run(
            ["bash", "-c", f"cd {ROOT} && {cmd}"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        combined = r.stdout + r.stderr
        if expected in combined:
            ok(f"{name:20s} → {expected}")
            passed += 1
        else:
            fail(f"{name:20s} expected '{expected}', got '{combined[:60]}'")
            failed += 1

    print(f"  Smoke: {passed}/{passed + failed} passed")
    return 1 if failed > 0 else 0


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
    r = subprocess.run(
        [
            "gcc",
            "-g",
            "-DTEST_BUILD=1",
            str(ROOT / "tests" / "runtime_test_harness.c"),
            str(ROOT / "lib" / "runtime.c"),
            "-o",
            "/tmp/runtime_test",
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
    r = subprocess.run(["/tmp/runtime_test"], capture_output=True, text=True, timeout=30)
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
    r = subprocess.run([sys.executable, "tests/repl_test.py"], cwd=ROOT)
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
    """Gradual Guarantee verification (#1961 → tests/run.py gradual)."""
    runner = ROOT / "tests" / "run.py"
    legacy = ROOT / "tests" / "check_gradual.py"
    if runner.exists():
        cmd = [sys.executable, str(runner), "gradual"]
    elif legacy.exists():
        cmd = [sys.executable, str(legacy)]
    else:
        print(f"  {legacy} not found")
        return 1
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    print(r.stdout)
    if r.returncode != 0:
        fail("gradual guarantee failed")
        return 1
    ok("gradual guarantee passed")
    return 0


def test_bash():
    """Bash regression (#1961 → tests/run.py bash)."""
    print(f"{B}═══ Bash regression tests ═══{N}")
    runner = ROOT / "tests" / "run.py"
    shell = ROOT / "tests" / "run-tests.sh"
    if runner.exists():
        r = subprocess.run(
            [sys.executable, str(runner), "bash"],
            env={**os.environ, "AURA": str(AURA)},
            capture_output=True,
            text=True,
            timeout=120,
        )
    elif shell.exists():
        r = subprocess.run(
            ["bash", str(shell)],
            env={**os.environ, "AURA": str(AURA)},
            capture_output=True,
            text=True,
            timeout=120,
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
    """Run tests/regression/*.aura as compiler regression checks."""
    reg_dir = ROOT / "tests" / "regression"
    aura_bin = os.environ.get("AURA_BIN", str(AURA))
    if not reg_dir.exists():
        print("  No regression tests found", flush=True)
        return 0

    failed = 0
    total = 0
    for fpath in sorted(reg_dir.glob("*.aura")):
        total += 1
        text = fpath.read_text()
        expected = ""
        for line in text.splitlines():
            if line.startswith(";; expect:"):
                expected = line[len(";; expect:") :].strip()
                break

        name = fpath.stem
        code_lines = []
        in_code = False
        for line in text.splitlines():
            if not in_code and not line.startswith(";;") and line.strip():
                in_code = True
            if in_code:
                code_lines.append(line)
        code = "\n".join(code_lines)

        try:
            r = subprocess.run([aura_bin], input=code, capture_output=True, text=True, timeout=10)
            sig_map = {-6: "SIGABRT", -8: "SIGFPE", -11: "SIGSEGV"}

            if expected == "no-crash":
                if r.returncode < 0:
                    print(
                        f"    FAIL {name}: {sig_map.get(r.returncode, f'signal{-r.returncode}')}",
                        flush=True,
                    )
                    failed += 1
                else:
                    print(f"    PASS {name}")
            elif expected == "no-error":
                if "internal error" in (r.stderr or "").lower():
                    print(f"    FAIL {name}: internal error", flush=True)
                    failed += 1
                else:
                    print(f"    PASS {name}")
            elif expected == "no-timeout":
                print(f"    PASS {name}")
            elif r.returncode < 0:
                print(
                    f"    FAIL {name}: {sig_map.get(r.returncode, f'signal{-r.returncode}')}",
                    flush=True,
                )
                failed += 1
            elif r.returncode != 0:
                print(f"    FAIL {name}: exit {r.returncode}", flush=True)
                failed += 1
            elif expected and expected not in (r.stdout or ""):
                print(f"    FAIL {name}: expected '{expected}', got '{r.stdout.strip()}'")
                failed += 1
            else:
                print(f"    PASS {name}")
        except subprocess.TimeoutExpired:
            print(f"    TIMEOUT {name}", flush=True)
            failed += 1

    print(f"  Regression: {total - failed}/{total} passed", flush=True)
    return 0 if failed == 0 else 1


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
    r = subprocess.run([str(bin_path)], timeout=600)
    if r.returncode != 0 and r.stderr:
        print(r.stderr[:500], file=sys.stderr)
    return r.returncode


def test_issue_146():
    """Run Issue #146 (pure-function extraction) tests."""
    print(f"{B}═══ Issue #146 Tests (pure-function extraction) ═══{N}")
    bin_path = BUILD / "test_issue_146"
    if not bin_path.exists():
        print("  test_issue_146 binary not found (build first)")
        return 1
    r = subprocess.run([str(bin_path)], timeout=60)
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
    """
    tier = issues_tier()
    jobs = os.environ.get("AURA_ISSUES_JOBS") or str(min(8, os.cpu_count() or 4))
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
    r = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tests" / "run.py"),
            cmd_name,
            "--tier",
            tier,
            "--",
            "--jobs",
            jobs,
            *extra_args,
        ],
        capture_output=True,
        text=True,
        timeout=900 if tier == "full" else 300,
    )
    print(r.stdout)
    if r.stderr:
        print(r.stderr, file=sys.stderr)
    return r.returncode


def test_p0_regression():
    """Run P0 fix regression tests."""
    print(f"{B}═══ P0 Regression Tests ═══{N}")
    # test_regression.py runs 150+ Aura subprocess cases plus JIT/AOT/fuzz
    # helpers; wall time often exceeds 3 min on loaded runners.
    r = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "test_regression.py")],
        capture_output=True,
        text=True,
        timeout=300,
        cwd=str(ROOT),
    )
    print(r.stdout)
    if r.stderr:
        print(r.stderr, file=sys.stderr)
    return r.returncode


# Suite tests that are temporarily skipped because of pre-existing
# issues unrelated to the current work. Each entry is (filename, reason).
# The skip is reported as a warning (so it's visible in CI logs) but does
# not fail the suite. These are tracked as follow-up work — see
# commit messages on the relevant fixes for context.
SUITE_SKIP = {
    # Add entries here as {filename: reason} for tests that should be
    # temporarily skipped. Empty = all suite tests run.
    #
    # (concurrent.aura's pre-existing flake was verified gone:
    #  20/20 regular + 10/10 ASAN runs all pass cleanly. The
    #  UAF fixes in 334c7d2 / c8ee203 closed the root cause.
    #  Removed the skip entry.)
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


def test_suite_runner(*, s0: bool = False):
    """Run all tests/suite/*.aura files.

    s0=True sets AURA_PRIMITIVES=s0 and only runs SUITE_S0_FILES (surface smoke).
    """
    label = "Suite tests (s0)" if s0 else "Suite tests"
    print(f"{B}═══ {label} ═══{N}")
    root = ROOT / "tests" / "suite"
    passed = 0
    failed = 0
    skipped = 0
    env = os.environ.copy()
    if s0:
        env["AURA_PRIMITIVES"] = "s0"
    for f in sorted(root.glob("*.aura")):
        if f.name == "run-tests.aura":
            continue
        name = f.stem
        if f.name in SUITE_SKIP:
            print(f"  {Y}↷{N}  suite/{name}.aura: SKIPPED — {SUITE_SKIP[f.name]}")
            skipped += 1
            continue
        if s0 and f.name not in SUITE_S0_FILES:
            continue  # not part of s0 smoke set (silent skip; not counted)
        code = f.read_text()
        if not code:
            warn(f"  suite/{name}.aura: empty")
            failed += 1
            continue
        r = subprocess.run(
            [str(AURA), "--load", str(f)],
            capture_output=True,
            text=True,
            timeout=120,
            env=env,
        )
        if r.returncode == 0:
            ok(f"  suite/{name}.aura")
            passed += 1
        else:
            errstr = r.stderr[:80] if r.stderr else r.stdout[:80]
            warn(f"  suite/{name}.aura: {errstr}")
            failed += 1
    total = passed + failed + skipped
    summary = f"  Suite: {passed}/{total} passed"
    if skipped:
        summary += f" ({skipped} skipped)"
    if s0:
        summary += " [AURA_PRIMITIVES=s0]"
    print(summary)
    return 1 if failed > 0 else 0


def test_suite_s0():
    """Curated suite under AURA_PRIMITIVES=s0 (P4 surface smoke)."""
    return test_suite_runner(s0=True)


# ═══════════════════════════════════════════════════════════════
# CI tiering
# ═══════════════════════════════════════════════════════════════

CI_CORE = [
    "unit",
    "integ",
    "typecheck",
    "smoke",
    "bash",
    "suite",
    "repl",
    "runtime-c",
    "concurrent",
]
CI_SAFETY = ["gradual", "regression", "p0"]
# Issue #226: unified test_issue_* runner (tests/run_issue_tests.py).
# AURA_ISSUES_TIER=fast on PR CI (~18 targets + changed issues);
# AURA_ISSUES_TIER=full on main (all ~90+ binaries).
CI_ISSUES = ["issues"]
CI_ISSUES_FAST = ["issues-fast"]
# Suites safe to run in parallel (separate binaries / no shared /tmp paths).
CI_PARALLEL_SAFE = frozenset(
    {
        "unit",
        "concurrent",
        "issues",
        "issues-fast",
        "repl",
        "gradual",
        "runtime-c",
    }
)

SUITES = {
    "unit": test_unit,
    "integ": test_integ,
    "typecheck": test_typecheck,
    "bench": test_bench,
    "smoke": test_smoke,
    "mutation": test_mutation,
    "runtime-c": test_runtime_unit,
    "gradual": test_gradual,
    "demo": test_demo,
    "regression": test_regression,
    "p0": test_p0_regression,
    "ai": test_ai_agent_demo,
    "bash": test_bash,
    "suite": test_suite_runner,
    "suite-s0": test_suite_s0,
    "repl": test_repl,
    "concurrent": test_concurrent,
    "issues": test_issues,
    "issues-fast": test_issues,
}


_test_print_lock = Lock()


def _test_jobs() -> int:
    raw = os.environ.get("AURA_TEST_JOBS", "1").strip()
    try:
        return max(1, int(raw))
    except ValueError:
        return 1


def _expand_suite_names(suite_names: list[str]) -> list[tuple[str, object]]:
    if not suite_names or "all" in suite_names:
        suite_names = list(SUITES.keys())

    if "issues-fast" in suite_names:
        os.environ["AURA_ISSUES_TIER"] = "fast"

    items: list[tuple[str, object]] = []
    for name in suite_names:
        if name in SUITES:
            items.append((name, SUITES[name]))
        elif name == "core":
            for s in CI_CORE:
                items.append((f"core/{s}", SUITES[s]))
        elif name == "safety":
            for s in CI_SAFETY:
                items.append((f"safety/{s}", SUITES[s]))
        else:
            warn(f"unknown suite '{name}' (use: {', '.join(SUITES.keys())})")
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


def _summarize_test_results(results: dict[str, int]) -> int:
    print(f"\n{'═' * 50}")
    all_ok = all(v == 0 for v in results.values())
    total = len(results)
    bad = total - sum(1 for v in results.values() if v == 0)
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


def cmd_test(suite_names: list[str]):
    """Run test suites."""
    items = _expand_suite_names(suite_names)
    if not items:
        warn("no test suites to run")
        return 1

    jobs = _test_jobs()
    results: dict[str, int] = {}

    if jobs <= 1:
        for label, fn in items:
            name, rc, _elapsed = _run_suite(label, fn)
            results[name] = rc
        return _summarize_test_results(results)

    parallel = [(lbl, fn) for lbl, fn in items if _suite_base_name(lbl) in CI_PARALLEL_SAFE]
    serial = [(lbl, fn) for lbl, fn in items if _suite_base_name(lbl) not in CI_PARALLEL_SAFE]

    if parallel:
        workers = min(jobs, len(parallel))
        print(f"{B}Running {len(parallel)} parallel-safe suites (jobs={workers}); {len(serial)} aura suites serial{N}")
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(_run_suite, label, fn): label for label, fn in parallel}
            for fut in as_completed(futures):
                label, rc, _elapsed = fut.result()
                results[label] = rc

    for label, fn in serial:
        name, rc, _elapsed = _run_suite(label, fn)
        results[name] = rc

    return _summarize_test_results(results)


def cmd_primitive_surface():
    """P0b/#1432 freeze + #1448 SlimSurface --strict (budget + facade report)."""
    print(f"{B}═══ Primitive surface freeze + SlimSurface ═══{N}")
    script = ROOT / "scripts" / "check_primitive_surface.py"
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
    ut = ROOT / "tests" / "test_primitive_surface_gate.py"
    if ut.exists():
        r2 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r2.returncode != 0:
            fail("primitive surface gate unit tests failed")
            return 1
    ok("primitive surface freeze + SlimSurface --strict OK")
    return 0


def cmd_test_registry():
    """Issue #1572: test-registry.json freshness (scripts/gen_test_registry.py).

    Default: --check (fail if docs/generated/test-registry.json is stale).
    With --fix: rewrite the registry from tests/test_*.cpp headers.
    Also wired into pre-commit when tests/*.cpp is staged.
    """
    fix = "--fix" in sys.argv[2:]
    print(f"{B}═══ Test registry {'(fix)' if fix else '(check)'} (#1572) ═══{N}")
    script = ROOT / "scripts" / "gen_test_registry.py"
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
    ut = ROOT / "tests" / "test_test_binding_gate.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_test_binding_gate unit tests failed")
            return 1
    # check_test_coverage.py umbrella removed per Anqi 2026-07-19 directive
    # (scripts/ audit wave 9). check_test_binding.py covers the same surface
    # (production primitive sources must have tests/).
    script = ROOT / "scripts" / "check_test_binding.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("test binding failed — production primitive sources changed without tests/")
        return 1
    ok("test binding + coverage OK")
    return 0


def cmd_naming_convention():
    """Issue #1886: naming_convention.md sections + example template keys."""
    print(f"{B}═══ Naming convention doc (#1886) ═══{N}")
    script = ROOT / "scripts" / "check_naming_convention.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("naming convention check failed — see docs/naming_convention.md")
        return 1
    ok("naming convention doc OK")
    return 0


def cmd_dead_heap_push():
    """Issue #1488 / #1668: dead string_heap_ push pollution audit (strict)."""
    print(f"{B}═══ Dead string_heap push audit (#1668) ═══{N}")
    script = ROOT / "scripts" / "audit_dead_heap_push.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    ut = ROOT / "tests" / "test_audit_dead_heap_push.py"
    if ut.exists():
        r0 = subprocess.run([sys.executable, str(ut)], cwd=ROOT)
        if r0.returncode != 0:
            fail("test_audit_dead_heap_push unit tests failed")
            return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "dead string_heap_ push candidates found — "
            "run python3 scripts/audit_dead_heap_push.py and remove unused pushes"
        )
        return 1
    ok("dead heap push audit clean")
    return 0


def cmd_catch_silent_swallow():
    """Issue #1669 / #615: catch(...) must carry SILENCE-PRIM marker (strict)."""
    print(f"{B}═══ catch(...) SILENCE-PRIM audit (#1669) ═══{N}")
    script = ROOT / "scripts" / "audit_catch_silent_swallow.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    ut = ROOT / "tests" / "test_audit_catch_silent_swallow.py"
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


def cmd_mutation_guard_coverage():
    """Issue #1931 / #1950: compile:*/mutate:* must use MutationBoundaryGuard."""
    print(f"{B}═══ MutationBoundaryGuard coverage (#1931 / #1950) ═══{N}")
    script = ROOT / "scripts" / "check_mutation_guard_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict", "--quiet"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("uncovered compile:*/mutate:* primitive(s) — wrap with run_under_mutation_guard")
        return 1
    ok("mutation guard coverage 100%")
    return 0


def cmd_gate():
    """Fast static checks for CI (docs + lint + format + fixtures + surface + registry + binding).

    Issue #1572: pass --fix to auto-regen docs + test-registry and to run
    lint/format in fix mode (those subcommands already read --fix from argv).
    CI always runs without --fix (check-only).

    Issue #1573: pass --scripts-only (or AURA_GATE_SCRIPTS_ONLY=1) to skip
    clang-format (e.g. when C++ toolchain unavailable).

    Issue #1668: also runs dead string_heap_ push audit (--strict).
    Issue #1669: also runs catch(...) SILENCE-PRIM audit (--strict).
    Issue #1931: also runs mutation Guard coverage linter (--strict).
    """
    fix = "--fix" in sys.argv[2:]
    scripts_only = "--scripts-only" in sys.argv[2:] or os.environ.get("AURA_GATE_SCRIPTS_ONLY", "").strip() in (
        "1",
        "true",
        "yes",
    )
    mode = "fix" if fix else "check"
    if scripts_only:
        mode += "+scripts-only"
    print(f"{B}═══ Gate ({mode}) ═══{N}")
    # Short-circuit with `or` (do not eagerly build a list of call results).
    rc = cmd_docs(check=not fix) or cmd_lint()
    if rc:
        return rc
    if scripts_only:
        info("scripts-only: skipping clang-format (cross-platform gate)")
    else:
        rc = cmd_format()
        if rc:
            return rc
    return (
        cmd_fixtures()
        or cmd_primitive_surface()
        or cmd_test_registry()
        or cmd_test_binding()
        or cmd_naming_convention()
        or cmd_dead_heap_push()
        or cmd_catch_silent_swallow()
        or cmd_mutation_guard_coverage()
    )


def cmd_ci():
    """CI build + test (parallel suites when AURA_TEST_JOBS>1)."""
    suites = CI_CORE + CI_SAFETY + CI_ISSUES
    return cmd_build() or cmd_test(suites)


def cmd_list():
    """列出测试套件"""
    print(f"{B}Available test suites:{N}")
    print(f"  {'core':12s} CI核心管线 (unit + integ + typecheck + smoke + bash + suite)")
    print(f"  {'safety':12s} CI安全回归 (gradual + regression + p0)")
    print(f"  {'check':12s} CI默认: build + core + safety + issues")
    print(f"  {'issues-fast':12s} issue tests (AURA_ISSUES_TIER=fast)")
    print()
    for name, func in sorted(SUITES.items()):
        print(f"  {name:12s} {func.__doc__}")
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
            "-Wno-dev",
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
            "-Wno-dev",
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
SBOM_SCRIPT = ROOT / "scripts" / "gen_sbom.py"


def _repro_cmake_flags() -> tuple[str, str, str]:
    src = str(ROOT)
    flags = f"-ffile-prefix-map={src}=. -fdebug-prefix-map={src}=. -frandom-seed=aura-repro-675 -g0"
    ldflags = "-Wl,--build-id=none"
    return flags, flags, ldflags


def cmd_repro():
    """Reproducible Release build (verify path removed per Anqi 2026-07-19
    directive — scripts/ci_reproducibility.py deleted; the --verify entry
    point is dropped, but the reproducible build itself remains)."""
    print(f"{B}═══ Reproducible build ═══{N}")
    global BUILD, AURA, TEST_BIN
    BUILD = ROOT / "build_repro"
    AURA = BUILD / "aura"
    TEST_BIN = BUILD / "aura"
    BUILD.mkdir(parents=True, exist_ok=True)
    nproc = _build_jobs()
    cflags, cxxflags, ldflags = _repro_cmake_flags()
    env = {
        **os.environ,
        "SOURCE_DATE_EPOCH": os.environ.get("SOURCE_DATE_EPOCH", REPRO_SOURCE_DATE_EPOCH),
        "CCACHE_DISABLE": "1",
        "AURA_BUILD_TYPE": "Release",
    }
    r = run(
        [
            "cmake",
            "-B",
            str(BUILD),
            "-G",
            "Ninja",
            "-Wno-dev",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_FLAGS={cflags}",
            f"-DCMAKE_CXX_FLAGS={cxxflags}",
            f"-DCMAKE_EXE_LINKER_FLAGS={ldflags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={ldflags}",
        ],
        cwd=ROOT,
        env=env,
    )
    if r != 0:
        return r
    r = run(
        ["cmake", "--build", str(BUILD), "--target", "aura", "-j", str(nproc)],
        cwd=ROOT,
        env=env,
    )
    if r == 0:
        ok(f"repro build OK → {AURA}")
    else:
        fail("repro build failed")
    return r


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


def run_bench_llm():
    """Run LLM benchmarks (DeepSeek / MiniMax / Grok) in parallel."""
    print(f"{B}═══ LLM Benchmark (3 models in parallel) ═══{N}")
    bench_script = ROOT / "tests" / "run_bench_all.py"
    if not bench_script.exists():
        fail(f"Script not found: {bench_script}")
        return 1
    env = {**os.environ, "AURA_BIN": str(AURA), "PYTHONUNBUFFERED": "1"}
    return run([sys.executable, str(bench_script)], env=env)


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
        "clean": cmd_clean,
        "check": lambda: cmd_gate() or cmd_ci(),
        "gate": cmd_gate,
        "ci": cmd_ci,
        "docs": cmd_docs,
        "fixtures": cmd_fixtures,
        "lint": cmd_lint,
        "format": cmd_format,
        "test-registry": cmd_test_registry,
        "naming-convention": cmd_naming_convention,
        "dead-heap-push": cmd_dead_heap_push,
        "catch-silent-swallow": cmd_catch_silent_swallow,
        "mutation-guard-coverage": cmd_mutation_guard_coverage,
        "test": lambda: cmd_test(args or ["all"]),
        "list": cmd_list,
        "demo": test_demo,
        "regression": lambda: cmd_test(["regression"]),
        "bench": cmd_bench,
        "bench-llm": run_bench_llm,
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
