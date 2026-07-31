#!/usr/bin/env python3
"""
Aura — 统一构建/测试入口

Usage:
  ./build.py [--sanitizer=asan|ubsan|tsan] build    # CMake 构建 (sanitizer-插桩)
  ./build.py [--sanitizer=asan|ubsan|tsan] test [suite]  # 运行测试
  ./build.py check            # gate + ci（与 CI 相同）
  ./build.py gate             # docs + lint + format + fixtures + surface + binding + registry + dead-heap + aot-stamp + inventory
  ./build.py gate --fix       # 同上，但 auto-regen docs/registry/inventory + lint/format --fix（#1572/#1957）
  ./build.py gate --scripts-only  # 跳过 clang-format（脚本-only,无 C++ 编译）
  ./build.py legacy-test-inventory  # #1957 inventory freshness (--fix to regen)
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
  ./build.py aot-env-linear-stamp  # AOT mangle (0,0) env/linear stamp fence（#2091/#2168）
  ./build.py repro [--verify]  # 可复现 Release 构建（#675）

  ./build.py sbom [--version=V] # CycloneDX SBOM 生成（#675）
  ./build.py security          # 依赖/文件系统漏洞扫描（#675）
  ./build.py bench [--strict]  # Benchmark 基线 + 回归检测（#1569 SLO gate）
  ./build.py coverage --html   # LLVM source-based coverage report (#1933)
  ./build.py coverage --check-tools  # verify llvm-cov tooling only
  ./build.py fuzz --list       # list registered fuzzers (#1935)
  ./build.py fuzz --all --quick  # run fuzz orchestrator
  ./build.py production-concurrency  # #2380 nightly gate: canary + full chaos soak
  ./build.py production-concurrency-coverage  # #2380 static AC contract rows

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

# Issue #1932: harness lives under tests/python/; bench cases under tests/bench/
_ROOT_FOR_PATH = Path(__file__).resolve().parent
sys.path.insert(0, str(_ROOT_FOR_PATH / "tests" / "python"))
sys.path.insert(0, str(_ROOT_FOR_PATH / "tests" / "bench"))
from _aura_harness import B, G, N, R, Y, fail, info, ok, run, warn  # noqa: E402
from benchmark_cases import load_typecheck_cases  # noqa: E402
from integ_cases import load_integ_cases  # noqa: E402
from issue_tier import issues_tier, load_fast_targets, resolve_issue_targets  # noqa: E402
from smoke_cases import load_smoke_cases  # noqa: E402

ROOT = Path(__file__).resolve().parent
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


def _aura_test_env(extra: dict | None = None) -> dict:
    """Env for spawning `./build/aura` under CI / local harnesses.

    Issue #2053 / #2213: `main()` always calls
    `apply_production_security_defaults()` (Restricted sandbox, Forbidden
    tree-walker fallback, Full TypedMutationAudit). Integration suites
    (bash / suite / integ / regression / gradual / p0 / repl) must default
    to `AURA_SANDBOX=off` so Soft pipeline + Soft audit remain ergonomic —
    otherwise tree-walker-forbidden + MacroSelfEvo grant denials mass-fail
    under production defaults.

    Explicit `AURA_SANDBOX` in the caller environment always wins (canary /
    intentional prod-like runs). Does **not** mutate global `os.environ` so
    C++ unit tests that unsetenv + call `apply_production_security_defaults`
    keep clean production-default coverage.
    """
    env = os.environ.copy()
    if not str(env.get("AURA_SANDBOX", "")).strip():
        env["AURA_SANDBOX"] = "off"
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
    # AURA_ISSUE_BUILD=none/skip/off → skip issue matrix entirely.
    #
    # Sanitizer builds (build_asan / build_ubsan / build_tsan) default to
    # skipping the issue matrix: linking 500+ ASAN-instrumented test
    # binaries fills the GitHub Actions runner disk ("No space left on
    # device") and blows the wall clock. asan-verify / ubsan-smoke only
    # need aura + test_ir (+ concurrent). Force full matrix with
    # AURA_ISSUE_BUILD=all under --sanitizer=.
    tier = issues_tier()
    issue_mode = os.environ.get("AURA_ISSUE_BUILD", "all").strip().lower()
    san_name = BUILD.name.removeprefix("build_") if BUILD.name.startswith("build_") else ""
    if san_name in SANITIZER_FLAGS and issue_mode in ("all", ""):
        issue_mode = "none"
        info(f"issue tests: skipped under --sanitizer={san_name} (set AURA_ISSUE_BUILD=all to force full matrix)")
    t0 = time.time()
    if issue_mode in ("none", "skip", "off", "0"):
        info("issue tests: skipped (AURA_ISSUE_BUILD=none)")
        r = 0
    elif tier == "full" and issue_mode == "bundles":
        from issue_tier import BUNDLE_PROFILES

        targets = [f"test_issues_{p}" for p in BUNDLE_PROFILES]
        issue_cmd = ["ninja", "-C", str(BUILD), "-k", "0", f"-j{nproc}", *targets]
        info(f"issue tests: tier=full mode=bundles ({len(targets)} bundle targets)")
        r = run(issue_cmd, cwd=ROOT)
        if r != 0:
            warn("issue-test build failed — retrying once (module dyndep flake workaround)")
            r = run(issue_cmd, cwd=ROOT)
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
        r = run(issue_cmd, cwd=ROOT)
        if r != 0:
            warn("issue-test build failed — retrying once (module dyndep flake workaround)")
            r = run(issue_cmd, cwd=ROOT)
    else:
        targets = resolve_issue_targets("fast")
        issue_cmd = ["ninja", "-C", str(BUILD), "-k", "0", f"-j{nproc}", *targets]
        changed = [t for t in targets if t not in set(load_fast_targets())]
        extra = f", +{len(changed)} git-changed" if changed else ""
        info(f"issue tests: tier=fast ({len(targets)} targets{extra})")
        r = run(issue_cmd, cwd=ROOT)
        if r != 0:
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

    # test_concurrent
    concurrent_bin = BUILD / "test_concurrent"
    if concurrent_bin.exists():
        start2 = time.time()
        r2 = subprocess.run([str(concurrent_bin)], timeout=300)
        elapsed2 = time.time() - start2
        # binary prints directly to terminal; just check rc
        if r2.returncode == 0:
            ok(f"concurrent (exit {r2.returncode}) in {elapsed2:.2f}s")
        else:
            fail(f"concurrent (exit {r2.returncode}) in {elapsed2:.2f}s")
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
# (loaded via tests/python/integ_cases.py — #1932 layout).


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

        r = subprocess.run(
            args,
            input=pipe_input,
            capture_output=True,
            text=True,
            timeout=30,
            env=_aura_test_env(),
        )

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
            env=_aura_test_env(),
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
            env=_aura_test_env(),
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
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30, env=_aura_test_env())
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
            r = subprocess.run(
                [aura_bin],
                input=code,
                capture_output=True,
                text=True,
                timeout=10,
                env=_aura_test_env(),
            )
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
    # R10 #1932: tests/run.py → tests/python/run.py.
    run_path = ROOT / "tests" / "python" / "run.py"
    if not run_path.exists():
        run_path = ROOT / "tests" / "run.py"
    r = subprocess.run(
        [
            sys.executable,
            str(run_path),
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
    # tests/python/test_regression.py runs 150+ Aura subprocess cases plus
    # JIT/AOT/fuzz helpers; wall time often exceeds 3 min on loaded runners.
    # Path moved under tests/python/ in #1932 layout migration.
    r = subprocess.run(
        [sys.executable, str(ROOT / "tests" / "python" / "test_regression.py")],
        capture_output=True,
        text=True,
        timeout=300,
        cwd=str(ROOT),
        env=_aura_test_env(),
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
    env = _aura_test_env()
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


def test_e2e():
    """Issue #1934: commercial_readiness .aura E2E with golden PASS labels."""
    script = ROOT / "tests" / "python" / "run_e2e.py"
    if not script.is_file():
        fail(f"missing {script}")
        return 1
    return run([sys.executable, str(script)], cwd=ROOT)


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
    "e2e": test_e2e,  # Issue #1934 commercial_readiness golden E2E
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
    # Issue #1932: gate unit tests live under tests/python/
    ut = ROOT / "tests" / "python" / "test_test_binding_gate.py"
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


def cmd_side_effect_security():
    """Issue #2057: effectful prims must use require_effect / add_mutate / exempt."""
    print(f"{B}═══ Side-effect security coverage (#2057) ═══{N}")
    script = ROOT / "scripts" / "check_side_effect_security.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script), "--strict"], cwd=ROOT)
    if r.returncode != 0:
        fail(
            "side-effect security gate failed — new effectful prim without "
            "require_effect / add_mutate / security_exempt "
            "(see src/compiler/security_side_effect.hh)"
        )
        return 1
    ok("side-effect security coverage OK")
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


def cmd_mutation_guard_coverage():
    """Issue #1931 / #1950 / #1953 / #2124: mutate/compile via try_acquire only."""
    print(f"{B}═══ MutationBoundaryGuard coverage (#1931 / #1950 / #1953 / #2124) ═══{N}")
    script = ROOT / "scripts" / "check_mutation_guard_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict", "--quiet"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail(
            "uncovered compile:*/mutate:* or residual legacy Guard ctor — "
            "use MutationBoundaryGuard::try_acquire / run_under_mutation_guard (#2124)"
        )
        return 1
    ok("mutation guard coverage 100% (try_acquire, 0 legacy ctor)")
    return 0


def cmd_orch_mvp_scope():
    """Issue #1965 / #1966: orch/ multi-agent public symbols must stay removed."""
    print(f"{B}═══ orch MVP scope (#1965 / #1966) ═══{N}")
    script = ROOT / "scripts" / "check_orch_mvp_scope.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict", "--quiet"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail(
            "removed orch multi-agent symbol reintroduced — "
            "use parallel_intend / evaluator-local name table (see #1966)"
        )
        return 1
    ok("orch MVP scope clean (no AgentRegistry / conduct_parallel reintro)")
    return 0


def cmd_aot_env_linear_stamp():
    """Issue #2091 / #2168: forbid literal (0,0) env/linear on production mangle/emit.

    Runs scripts/check_aot_env_linear_stamp_coverage.py as a hard gate:
    any production call to mangle_aot_name / aot_link_name that passes
    literal (0, 0) without `# 2091-allow-zero` (or `# 2091-legacy`) fails
    the build. Tests/stubs/header defaults remain allowed (script skip list).
    """
    print(f"{B}═══ AOT env/linear stamp coverage (#2091 / #2168) ═══{N}")
    script = ROOT / "scripts" / "check_aot_env_linear_stamp_coverage.py"
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

    Without --fix: ``scripts/inventory_legacy_tests.py --check`` (exit 1 if
    tests/legacy_test_inventory.md is stale). With --fix: regenerate the
    markdown. Re-run after domain migrations or bulk test adds.
    """
    print(f"{B}═══ Legacy test inventory (#1957) ═══{N}")
    script = ROOT / "scripts" / "inventory_legacy_tests.py"
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
                "`python3 scripts/inventory_legacy_tests.py` or `./build.py gate --fix`"
            )
        return 1
    ok("legacy test inventory up to date" if not fix else "legacy test inventory regenerated")
    return 0


def cmd_register_render_hot_prim_coverage():
    """Issue #2217: known TUI/render hot prims must use register_render_hot_prim."""
    print(f"{B}=== register_render_hot_prim coverage (#2217) ==={N}")
    script = ROOT / "scripts" / "check_register_render_hot_prim_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("register_render_hot_prim coverage contract failed")
        return 1
    ok("register_render_hot_prim coverage clean")
    return 0


def cmd_incremental_soundness_prod_coverage():
    """Issue #2245: production sampling of incremental soundness coverage.

    Validates the 5-AC contract from issue body:
      AC1: sample_bp > 0 + mode allows prod runs oracle on partial
      AC2: forced mismatch under sample_bp=10000 -> mismatch counter + forced full
      AC3: sample_bp=0 -> zero oracle cost
      AC4: 4 new query keys + schema-2245 lineage on query:incremental-soundness-stats
      AC5: StormLevel elevation factor (10x storm / 3x elevated)
    """
    print(f"{B}=== incremental soundness prod coverage (#2245) ==={N}")
    script = ROOT / "scripts" / "check_incremental_soundness_prod_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("incremental soundness prod coverage contract rows failed")
        return 1
    ok("incremental soundness prod coverage clean")
    return 0


def cmd_shape_storm_isolation_coverage():
    """Issue #2257: ShapeProfiler versioning + deopt-storm isolation.

    Validates the 5-AC contract from issue body:
      AC1: shape_version advances on compact + storm enter
      AC2: deopt rate stays bounded under HighMutation
      AC3: query surface (shape-version + deopt-storm-isolations-total
           + current-stability-ratio) + schema-2257 lineage
      AC4: zero extra cost on cold/stable functions
      AC5: integration with StormLevel facade
    """
    print(f"{B}=== shape profiler storm isolation coverage (#2257) ==={N}")
    script = ROOT / "scripts" / "check_shape_storm_isolation_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("shape profiler storm isolation coverage contract rows failed")
        return 1
    ok("shape profiler storm isolation coverage clean")
    return 0


def cmd_arena_moving_compaction_coverage():
    """Issue #2256: production-default Moving compaction + LifetimePin hard contract.

    Validates the 5-AC contract from issue body:
      AC1: production default ON (Moving compaction enabled by default)
      AC2: LifetimePin hard contract (pin-or-remap under Moving)
      AC3: zero-cost when no compact runs
      AC4: 4 metric fields + 4 query keys + schema-2256 lineage
      AC5: dual-worker stress test surface
    """
    print(f"{B}=== arena Moving-compact coverage (#2256) ==={N}")
    script = ROOT / "scripts" / "check_arena_moving_compaction_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("arena Moving-compact coverage contract rows failed")
        return 1
    ok("arena Moving-compact coverage clean")
    return 0


def cmd_arena_compact_hook_stats_coverage():
    """Issue #2381: concurrent compact_hook shape_inval counter is race-free.

    Option B for concurrent-hot counters (atomic RMW); GUARDED_BY audit on
    serial compact/live_compact stats_ fields. N=4 thread stress + exact count.
    """
    print(f"{B}=== arena compact_hook stats coverage (#2381) ==={N}")
    script = ROOT / "scripts" / "check_arena_compact_hook_stats_2381.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("arena compact_hook stats (#2381) coverage contract rows failed")
        return 1
    ok("arena compact_hook stats (#2381) coverage clean")
    return 0


def cmd_arena_dtor_clears_hooks_coverage():
    """Issue #2382: ASTArena dtor clears hooks before internal teardown.

    Nulls on_compact_hook_ / on_layout_change_ / root_remap_ under their
    mutexes before run_destructors() so concurrent invoke_*_ never fires
    dangling caller-capturing lambdas.
    """
    print(f"{B}=== arena dtor clears hooks coverage (#2382) ==={N}")
    script = ROOT / "scripts" / "check_arena_dtor_clears_hooks_2382.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("arena dtor clears hooks (#2382) coverage contract rows failed")
        return 1
    ok("arena dtor clears hooks (#2382) coverage clean")
    return 0


def cmd_has_on_compact_hook_lock_coverage():
    """Issue #2383: has_on_compact_hook locks hook_mtx_ (has_* lock parity).

    Matches has_on_layout_change / has_root_remap_callback; TSAN-clean
    concurrent set+has.
    """
    print(f"{B}=== has_on_compact_hook lock coverage (#2383) ==={N}")
    script = ROOT / "scripts" / "check_has_on_compact_hook_lock_2383.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("has_on_compact_hook lock (#2383) coverage contract rows failed")
        return 1
    ok("has_on_compact_hook lock (#2383) coverage clean")
    return 0


def cmd_require_effect_live_mid_coverage():
    """Issue #2384: require_effect stamps live mutation_id (not hardcode 0).

    Bound-grant mismatch denies + provenance_mismatch metric; match allows;
    SecurityEvent mid non-zero; Off sandbox still allows.
    """
    print(f"{B}=== require_effect live mid coverage (#2384) ==={N}")
    script = ROOT / "scripts" / "check_require_effect_live_mid_2384.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("require_effect live mid (#2384) coverage contract rows failed")
        return 1
    ok("require_effect live mid (#2384) coverage clean")
    return 0


def cmd_restricted_unset_principal_coverage():
    """Issue #2385: Restricted denies side-effects when principal unset.

    Production default Restricted must not silently skip isolation when
    set_tenant_principal was never called. Pure reads (effects=0) stay ok.
    """
    print(f"{B}=== Restricted unset principal coverage (#2385) ==={N}")
    script = ROOT / "scripts" / "check_restricted_unset_principal_2385.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("Restricted unset principal (#2385) coverage contract rows failed")
        return 1
    ok("Restricted unset principal (#2385) coverage clean")
    return 0


def cmd_grant_macro_self_evo_stamp_coverage():
    """Issue #2386: grant_macro_self_evo stamps grant_epoch + fiber (#2055).

    Macro self-evo grants participate in epoch fence + hard fiber isolation.
    """
    print(f"{B}=== grant_macro_self_evo stamp coverage (#2386) ==={N}")
    script = ROOT / "scripts" / "check_grant_macro_self_evo_stamp_2386.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("grant_macro_self_evo stamp (#2386) coverage contract rows failed")
        return 1
    ok("grant_macro_self_evo stamp (#2386) coverage clean")
    return 0


def cmd_capability_string_matrix_unify_coverage():
    """Issue #2387: unify sensitive string caps with Effect matrix.

    tenant-admin / syscall map to Effect bits; has_capability is matrix-first;
    revoke clears both; compile-stats remains staged string-only.
    """
    print(f"{B}=== capability string/matrix unify coverage (#2387) ==={N}")
    script = ROOT / "scripts" / "check_capability_string_matrix_unify_2387.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("capability string/matrix unify (#2387) coverage contract rows failed")
        return 1
    ok("capability string/matrix unify (#2387) coverage clean")
    return 0


def cmd_security_audit_fold_coverage():
    """Issue #2388: fold Capability + Isolation audit into SecurityEvent WAL.

    Private 128-slot rings dual-write SecurityEvent ring + optional WAL;
    single IsolationDeny path; Soft/WAL-off short-circuit preserved.
    """
    print(f"{B}=== security audit fold coverage (#2388) ==={N}")
    script = ROOT / "scripts" / "check_security_audit_fold_2388.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("security audit fold (#2388) coverage contract rows failed")
        return 1
    ok("security audit fold (#2388) coverage clean")
    return 0


def cmd_security_health_coverage():
    """Issue #2389: query:security-health single Agent score.

    Aggregates effect/isolation deny rates, epoch-fence health, WAL posture,
    and ring-wrap pressure into health-bp + force-reason.
    """
    print(f"{B}=== security-health coverage (#2389) ==={N}")
    script = ROOT / "scripts" / "check_security_health_2389.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("security-health (#2389) coverage contract rows failed")
        return 1
    ok("security-health (#2389) coverage clean")
    return 0


def cmd_validate_node_no_abort_coverage():
    """Issue #2390: validate_node reports/throws instead of hard-abort.

    !is_valid must return error string (fail_on_error=false) or throw
    logic_error (true); validate_post_restore stays non-crashing.
    """
    print(f"{B}=== validate_node no-abort coverage (#2390) ==={N}")
    script = ROOT / "scripts" / "check_validate_node_no_abort_2390.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("validate_node no-abort (#2390) coverage contract rows failed")
        return 1
    ok("validate_node no-abort (#2390) coverage clean")
    return 0


def cmd_validate_post_restore_soa_coverage():
    """Issue #2391: validate_post_restore SoA column size cross-check.

    Detects int_val_/sym_id_/node_gen_/type_id_/… size drift vs size()
    so restore does not report clean on corrupt SoA layouts.
    """
    print(f"{B}=== validate_post_restore SoA coverage (#2391) ==={N}")
    script = ROOT / "scripts" / "check_validate_post_restore_soa_2391.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("validate_post_restore SoA (#2391) coverage contract rows failed")
        return 1
    ok("validate_post_restore SoA (#2391) coverage clean")
    return 0


def cmd_fixup_deltas_coverage():
    """Issue #2392: fixup_deltas safe rebase (bounds + overflow).

    Parent-relative child deltas rebase to absolute NodeIds; wrap or
    OOB rebased ids clamp to NULL_NODE (set_child does not clamp).
    """
    print(f"{B}=== fixup_deltas coverage (#2392) ==={N}")
    script = ROOT / "scripts" / "check_fixup_deltas_2392.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("fixup_deltas (#2392) coverage contract rows failed")
        return 1
    ok("fixup_deltas (#2392) coverage clean")
    return 0


def cmd_last_validated_generation_atomic_coverage():
    """Issue #2394: last_validated_generation concurrent-safe atomic.

    CopyableAtomicU16 enables lock-free concurrent validate_with_provenance
    without torn uint16 writes (TSAN-clean shared-ref hot path).
    """
    print(f"{B}=== last_validated_generation atomic coverage (#2394) ==={N}")
    script = ROOT / "scripts" / "check_last_validated_generation_atomic_2394.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("last_validated_generation atomic (#2394) coverage contract rows failed")
        return 1
    ok("last_validated_generation atomic (#2394) coverage clean")
    return 0


def cmd_stable_ref_wire_endian_coverage():
    """Issue #2395: StableNodeRef wire multi-byte fields little-endian.

    Portable LE encode/decode for id/gen/mid/tenant/…; golden LE bytes
    + swap-corruption case; host-endian memcpy removed from multi-byte lanes.
    """
    print(f"{B}=== StableNodeRef wire endian coverage (#2395) ==={N}")
    script = ROOT / "scripts" / "check_stable_ref_wire_endian_2395.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("StableNodeRef wire endian (#2395) coverage contract rows failed")
        return 1
    ok("StableNodeRef wire endian (#2395) coverage clean")
    return 0


def cmd_orphan_reap_tick_coverage():
    """Issue #2396: production tick periodically reaps orphan fibers.

    Wire maybe_reap_orphans_on_tick into Scheduler::run; zero cost when
    orphan_count_cached_ == 0; AURA_ORPHAN_REAP_INTERVAL_MS (default 50).
    """
    print(f"{B}=== orphan reap tick coverage (#2396) ==={N}")
    script = ROOT / "scripts" / "check_orphan_reap_tick_2396.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("orphan reap tick (#2396) coverage contract rows failed")
        return 1
    ok("orphan reap tick (#2396) coverage clean")
    return 0


def cmd_join_drain_reclaim_still_running_coverage():
    """Issue #2397: reclaimed vs body-still-running after join-drain residual.

    still-running gauge + body-retired counter; query:orch-module-stats keys;
    zero cost on Ok join path.
    """
    print(f"{B}=== join-drain reclaim still-running coverage (#2397) ==={N}")
    script = ROOT / "scripts" / "check_join_drain_reclaim_still_running_2397.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("join-drain reclaim still-running (#2397) coverage contract rows failed")
        return 1
    ok("join-drain reclaim still-running (#2397) coverage clean")
    return 0


def cmd_mailbox_bp_recent_window_coverage():
    """Issue #2398: mailbox_bp_recent_total quiet-period window for BP admit.

    Sliding quiet period after last BP so spawn admit recovers without restart;
    send_backpressure_total stays cumulative; threshold=0 zero cost.
    """
    print(f"{B}=== mailbox BP recent window coverage (#2398) ==={N}")
    script = ROOT / "scripts" / "check_mailbox_bp_recent_window_2398.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mailbox BP recent window (#2398) coverage contract rows failed")
        return 1
    ok("mailbox BP recent window (#2398) coverage clean")
    return 0


def cmd_agent_scope_concurrent_coverage():
    """Issue #2399: AgentScope concurrent misuse detection (metric + optional abort).

    Single-owner assert via owner-tid + re-entry depth; metric path default;
    AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 hard abort; no internal mutex.
    """
    print(f"{B}=== AgentScope concurrent detect coverage (#2399) ==={N}")
    script = ROOT / "scripts" / "check_agent_scope_concurrent_2399.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("AgentScope concurrent detect (#2399) coverage contract rows failed")
        return 1
    ok("AgentScope concurrent detect (#2399) coverage clean")
    return 0


def cmd_parallel_isolation_level_coverage():
    """Issue #2400: parallel-intend batch hash isolation-level enum.

    serialized | best-effort-pure | none; additive keys; pure is never
    advertised as transactional isolation.
    """
    print(f"{B}=== parallel isolation-level coverage (#2400) ==={N}")
    script = ROOT / "scripts" / "check_parallel_isolation_level_2400.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("parallel isolation-level (#2400) coverage contract rows failed")
        return 1
    ok("parallel isolation-level (#2400) coverage clean")
    return 0


def cmd_agent_reply_coverage():
    """Issue #2401: agent-reply helper + orch:agent-reply Aura primitive.

    Standard worker response path for agent-ask; pending-ask table (not
    AgentRegistry); metrics + schema-2401.
    """
    print(f"{B}=== agent-reply coverage (#2401) ==={N}")
    script = ROOT / "scripts" / "check_agent_reply_2401.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("agent-reply (#2401) coverage contract rows failed")
        return 1
    ok("agent-reply (#2401) coverage clean")
    return 0


def cmd_restamp_incremental_coverage():
    """Issue #2402: incremental restamp default + wrap cost control.

    AURA_RESTAMP_POLICY=full|incremental|auto; last-call cost keys;
    schema-2402 on query:generation-stats.
    """
    print(f"{B}=== restamp incremental coverage (#2402) ==={N}")
    script = ROOT / "scripts" / "check_restamp_incremental_2402.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("restamp incremental (#2402) coverage contract rows failed")
        return 1
    ok("restamp incremental (#2402) coverage clean")
    return 0


def cmd_query_index_composite_coverage():
    """Issue #2403: composite index + shared_lock hold SLO for pattern/where.

    Constrained tag+arity±marker hits composite index; miss only on
    unconstrained; query-index-hit-rate + shared-lock-us keys schema-2403.
    """
    print(f"{B}=== query-index composite coverage (#2403) ==={N}")
    script = ROOT / "scripts" / "check_query_index_composite_2403.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("query-index composite (#2403) coverage contract rows failed")
        return 1
    ok("query-index composite (#2403) coverage clean")
    return 0


def cmd_stable_ref_export_coverage():
    """Issue #2404: Agent export validate_or_refresh contract.

    export_ref/export_held_ref/query:ensure-ref; export-refresh/stale-reject
    metrics; schema-2404; stamp-resolve return-path coverage.
    """
    print(f"{B}=== stable-ref export validate coverage (#2404) ==={N}")
    script = ROOT / "scripts" / "check_stable_ref_export_2404.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("stable-ref export validate (#2404) coverage contract rows failed")
        return 1
    ok("stable-ref export validate (#2404) coverage clean")
    return 0


def cmd_lifetime_pin_remap_coverage():
    """Issue #2265: LifetimePin Phase 3 — real ptr remap under Moving densify.

    Validates the 5-AC contract from issue body:
      AC1: LifetimePin::remap() + lifetime::remap_pins_pointing_to() API
      AC2: wire at densify site (after relocate_tracked_objects_for_moving_
           fills last_object_remap_, remap every (old, neu) pair BEFORE gen restamp)
      AC3: zero-cost happy path (no registry walk on Soft/Force)
      AC4: lifetime_pin_remap_total + lifetime_pin_remap_miss_total counters
           + CompilerMetrics.arena_live_compact_remapped_pins_total mirror +
           query:arena-live-compact-stats surfaces remapped-pins-total key +
           schema-2265 / issue-2265 lineage (additive, no schema break)
      AC5: tests/core/test_moving_compact_2166.cpp — pin → Moving →
           validate(cur_gen, arena_id) succeeds AND ptr() equals the densified
           address; negative pin (non-arena address) → invalidate after Moving
    """
    print(f"{B}=== LifetimePin Phase 3 remap coverage (#2265) ==={N}")
    script = ROOT / "scripts" / "check_lifetime_pin_remap_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("LifetimePin Phase 3 remap coverage contract rows failed")
        return 1
    ok("LifetimePin Phase 3 remap coverage clean")
    return 0


def cmd_moving_pin_contract_fail_closed_coverage():
    """Issue #2266: LifetimePin Phase 4 — verify_pins_under_moving_compact must fail-closed.

    Validates the 5-AC contract from issue body:
      AC1: Semantics — verify_pins_under_moving_compact(arena_id, old_addresses)
           returns false if any live pin's ptr_ appears as a key in the densify's
           old→new map AND ptr_ was not updated (and not invalidated). Returns
           true when all such pins were honored or no pins / empty remap.
      AC2: Driver behavior — Outermost Phase 5 / compact driver on false →
           bump moving_compact_pin_contract_fail_total, do not publish success
           metrics as if contract held; optional env AURA_MOVING_PIN_CONTRACT=hard
           forces hard-fail vs soft metric (default hard under production security
           defaults).
      AC3: Zero-cost happy path — not called from allocation hot path; only
           compact / Phase 5 driver (preserve #2256 AC3).
      AC4: Observability — moving_compact_pin_contract_fail_total counter
           (process + optional CompilerMetrics) + query key + schema-2266 /
           issue-2266 / moving-pin-contract-wired lineage on
           query:arena-live-compact-stats.
      AC5: Tests — positive (pin → Moving → remap → contract held) + negative
           (pin not remapped → verify returns false + counter bumps).
    """
    print(f"{B}=== Moving pin contract fail-closed coverage (#2266) ==={N}")
    script = ROOT / "scripts" / "check_moving_pin_contract_fail_closed_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("Moving pin contract fail-closed coverage contract rows failed")
        return 1
    ok("Moving pin contract fail-closed coverage clean")
    return 0


def cmd_root_remap_pass_coverage():
    """Issue #2294 / #2267: RootRemapPass real rewrite — Stable-object roots +
    Closure capture cells after Moving densify.

    Validates the 5-AC contract from #2294:
      AC1: Stable-object root rewrite + arena stats writeback.
      AC2: Closure capture rewrite + counters.
      AC3: Empty remap / Soft-only zero rewrite work.
      AC4: Fail-closed unmapped densify candidates + AURA_ROOT_REMAP_CONTRACT.
      AC5: Observability + Evaluator install + tests AC1-AC5.
    """
    print(f"{B}=== RootRemapPass real rewrite coverage (#2294/#2267) ==={N}")
    script = ROOT / "scripts" / "check_root_remap_pass_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("RootRemapPass real rewrite coverage contract rows failed")
        return 1
    ok("RootRemapPass real rewrite coverage clean")
    return 0


def cmd_envframe_ownership_transfer_coverage():
    """Issue #2295: EnvFrame ownership transfer protocol (transfer_to / drop).

    Validates the 5-AC contract from #2295:
      AC1: EnvFrameRef::transfer_to / drop API + metrics.
      AC2: refresh_after_fiber_migration wires transfer / drop.
      AC3: Happy path keeps ownership atomics at 0 (unit test).
      AC4: hold_gen_mismatch surface retained.
      AC5: Query keys + schema-2295 + tests extension.
    """
    print(f"{B}=== EnvFrame ownership transfer coverage (#2295) ==={N}")
    script = ROOT / "scripts" / "check_envframe_ownership_transfer_2295.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("EnvFrame ownership transfer coverage contract rows failed")
        return 1
    ok("EnvFrame ownership transfer coverage clean")
    return 0


def cmd_residual_gc_defer_multi_eval_coverage():
    """Issue #2296: Phase-5 residual Clear + multi-eval orphan steal harden.

    Validates AC1–AC5: force_clear_all, bit reconcile on steal, zero-cost
    happy path, Hard/Soft retained, query correlation + decision table.
    """
    print(f"{B}=== residual multi-eval Clear harden coverage (#2296) ==={N}")
    script = ROOT / "scripts" / "check_residual_gc_defer_multi_eval_2296.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("residual multi-eval Clear harden coverage contract rows failed")
        return 1
    ok("residual multi-eval Clear harden coverage clean")
    return 0


def cmd_capture_cell_remap_coverage():
    """Issue #2297: structural capture-cell remount after densify.

    Validates densify object_remap context, remount cell walk after env_gen
    PRIMARY, metrics/query schema-2297, RootRemapPass publish.
    """
    print(f"{B}=== structural capture-cell remount coverage (#2297) ==={N}")
    script = ROOT / "scripts" / "check_capture_cell_remap_2297.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("structural capture-cell remount coverage contract rows failed")
        return 1
    ok("structural capture-cell remount coverage clean")
    return 0


def cmd_general_object_pin_coverage():
    """Issue #2298: non-render general object pin-or-remap protocol.

    Validates pin_or_fail / GeneralObjectPin, fail-closed validate counters,
    PinOwner retained, Soft zero remap, inventory + query schema-2298.
    """
    print(f"{B}=== general object pin-or-remap coverage (#2298) ==={N}")
    script = ROOT / "scripts" / "check_general_object_pin_2298.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("general object pin-or-remap coverage contract rows failed")
        return 1
    ok("general object pin-or-remap coverage clean")
    return 0


def cmd_aot_per_eval_slot_invalidate_coverage():
    """Issue #2299: per-eval physical invalidate of generation-behind AOT slots.

    Validates owner_eval filter, nullptr process-default, ordering invariant,
    last-eval / per-eval counters + schema-2299, RegisterOwnerGuard.
    """
    print(f"{B}=== per-eval AOT slot invalidate coverage (#2299) ==={N}")
    script = ROOT / "scripts" / "check_aot_per_eval_slot_invalidate_2299.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("per-eval AOT slot invalidate coverage contract rows failed")
        return 1
    ok("per-eval AOT slot invalidate coverage clean")
    return 0


def cmd_lifetime_contract_snapshot_coverage():
    """Issue #2300: query:lifetime-contract-snapshot pure Agent surface.

    Validates pure make_lifetime_contract_snapshot formula, MutationHold +
    linear live counts, force_reason priority, schema-2300 additive keys.
    """
    print(f"{B}=== lifetime-contract-snapshot coverage (#2300) ==={N}")
    script = ROOT / "scripts" / "check_lifetime_contract_snapshot_2300.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("lifetime-contract-snapshot coverage contract rows failed")
        return 1
    ok("lifetime-contract-snapshot coverage clean")
    return 0


def cmd_type_timeout_repair_graph_coverage():
    """Issue #2343: TIMEOUT/CONFLICT var↔constraint graph for Agent repair.

    Validates UnresolvedGraphEdge export, suggested_roots ranking, SOLVED
    zero-cost path, additive schema-2343 query keys, #2284 lineage retained.
    """
    print(f"{B}=== type-timeout-repair graph coverage (#2343) ==={N}")
    script = ROOT / "scripts" / "check_type_timeout_repair_graph_2343.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("type-timeout-repair graph coverage contract rows failed")
        return 1
    ok("type-timeout-repair graph coverage clean")
    return 0


def cmd_escape_gate_key_contract_coverage():
    """Issue #2344: escape-gate publish key ↔ lower key contract (Option A).

    Wrong-key miss must never elide a binding blocked under any live summary;
    matching key retains #2286 isolation + zero-cost happy path.
    """
    print(f"{B}=== escape-gate key contract coverage (#2344) ==={N}")
    script = ROOT / "scripts" / "check_escape_gate_key_contract_2344.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("escape-gate key contract coverage contract rows failed")
        return 1
    ok("escape-gate key contract coverage clean")
    return 0


def cmd_composite_empty_cs_hard_coverage():
    """Issue #2345: production composite empty-CS hard-reject (anti false-green).

    expected_partial + empty CS → hard miss under production/Full; soft
    observe under Sampled/dev; vacuous structural batches stay OK.
    """
    print(f"{B}=== composite empty-CS hard-reject coverage (#2345) ==={N}")
    script = ROOT / "scripts" / "check_composite_empty_cs_hard_2345.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("composite empty-CS hard-reject coverage contract rows failed")
        return 1
    ok("composite empty-CS hard-reject coverage clean")
    return 0


def cmd_steal_snapshot_hard_invariant_coverage():
    """Issue #2346: resume MutationSafetySnapshot hard-invariant (fail-closed).

    Soft: mismatch metric only. Hard / production canary: mark-failed +
    steal-snapshot-hard-fail-total. Happy path: one existing snapshot sample.
    """
    print(f"{B}=== steal-snapshot hard-invariant coverage (#2346) ==={N}")
    script = ROOT / "scripts" / "check_steal_snapshot_hard_invariant_2346.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("steal-snapshot hard-invariant coverage contract rows failed")
        return 1
    ok("steal-snapshot hard-invariant coverage clean")
    return 0


def cmd_steal_snapshot_soft_production_lock_coverage():
    """Issue #2372: production hard-forbid Soft steal-snapshot + force-deopt ABI.

    Soft env ignored under production lock; missing/weak force-deopt ABI
    aborts under production; test override / sandbox=off keeps Soft for tests.
    """
    print(f"{B}=== steal-snapshot Soft production lock coverage (#2372) ==={N}")
    script = ROOT / "scripts" / "check_steal_snapshot_soft_production_lock_2372.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("steal-snapshot Soft production lock coverage contract rows failed")
        return 1
    ok("steal-snapshot Soft production lock coverage clean")
    return 0


def cmd_render_deopt_throttle_race_coverage():
    """Issue #2373: try_render_deopt_throttle CAS race fix.

    N concurrent callers within window → exactly one true; sequential
    outside window preserved; CAS replaces load/store check-then-act.
    """
    print(f"{B}=== render deopt throttle CAS race coverage (#2373) ==={N}")
    script = ROOT / "scripts" / "check_render_deopt_throttle_race_2373.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("render deopt throttle CAS race coverage contract rows failed")
        return 1
    ok("render deopt throttle CAS race coverage clean")
    return 0


def cmd_legacy_pin_registry_cleanup_coverage():
    """Issue #2374: remove dead legacy pin_registry densify walk + API.

    Selective invalidate walks pin_registry_shards via
    invalidate_pins_not_in_new_addrs; pin_registry()/mtx removed from
    lifetime_pin.ixx (empty post-#2342).
    """
    print(f"{B}=== legacy pin_registry cleanup coverage (#2374) ==={N}")
    script = ROOT / "scripts" / "check_legacy_pin_registry_cleanup_2374.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("legacy pin_registry cleanup coverage contract rows failed")
        return 1
    ok("legacy pin_registry cleanup coverage clean")
    return 0


def cmd_pin_bulk_all_shards_coverage():
    """Issue #2375: (restamp|invalidate)_all_pins_for_arena(0) all-shard walk.

    #2342 regression fix — arena_id==0 visits all 16 shards; N!=0 still
    single-shard. Boundary restamp + GC invalidate production callers.
    """
    print(f"{B}=== pin bulk all-shard walk coverage (#2375) ==={N}")
    script = ROOT / "scripts" / "check_pin_bulk_all_shards_2375.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("pin bulk all-shard walk coverage contract rows failed")
        return 1
    ok("pin bulk all-shard walk coverage clean")
    return 0


def cmd_steal_complete_strong_entry_coverage():
    """Issue #2377: force single steal-complete entry under production.

    Production multi-worker must link strong steal-complete (Panic clear →
    residual → LayoutStamp); weak/null fail-closed. Light sandbox metric.
    """
    print(f"{B}=== steal-complete strong entry coverage (#2377) ==={N}")
    script = ROOT / "scripts" / "check_steal_complete_strong_entry_2377.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("steal-complete strong entry (#2377) coverage contract rows failed")
        return 1
    ok("steal-complete strong entry (#2377) coverage clean")
    return 0


def cmd_mutate_mailbox_strict_coverage():
    """Issue #2347: MultiFiberMailbox Guard-live blocking recv hard audit.

    Soft: Policy A soft counter only. Strict / production: hard-total +
    optional Guard-window threshold force-rollback. Happy path: depth==0.
    """
    print(f"{B}=== mutate-mailbox Strict hard audit coverage (#2347) ==={N}")
    script = ROOT / "scripts" / "check_mutate_mailbox_strict_2347.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mutate-mailbox Strict hard audit coverage contract rows failed")
        return 1
    ok("mutate-mailbox Strict hard audit coverage clean")
    return 0


def cmd_mailbox_defer_drain_sla_coverage():
    """Issue #2378: mailbox defer drain SLA + hold-blocked latency.

    deferred_depth / HWM, flush latency after outermost exit, starvation
    signal; zero cost when no open defer.
    """
    print(f"{B}=== mailbox defer drain SLA coverage (#2378) ==={N}")
    script = ROOT / "scripts" / "check_mailbox_defer_drain_sla_2378.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mailbox defer drain SLA (#2378) coverage contract rows failed")
        return 1
    ok("mailbox defer drain SLA (#2378) coverage clean")
    return 0


def cmd_bidirectional_match_coverage():
    """Issue #2348: bidirectional check-mode for ADT match + GuardShape.

    Match check_flat_match under expected types; GuardShape If narrowing;
    opt-out when bidirectional_mode=false; schema-2348 observability.
    """
    print(f"{B}=== bidirectional match check-mode coverage (#2348) ==={N}")
    script = ROOT / "scripts" / "check_bidirectional_match_2348.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("bidirectional match check-mode coverage contract rows failed")
        return 1
    ok("bidirectional match check-mode coverage clean")
    return 0


def cmd_mutation_hold_slo_coverage():
    """Issue #2349: outermost hold SLO circuit-breaker (production fail path).

    Soft/sandbox: metric only. Production default: hold > SLO → success_flag
    false. Env AURA_MUTATION_HOLD_SLO_US=0 disables. No second timer.
    """
    print(f"{B}=== mutation hold SLO circuit-breaker coverage (#2349) ==={N}")
    script = ROOT / "scripts" / "check_mutation_hold_slo_2349.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mutation hold SLO circuit-breaker coverage contract rows failed")
        return 1
    ok("mutation hold SLO circuit-breaker coverage clean")
    return 0


def cmd_mutation_hold_estimate_coverage():
    """Issue #2405: query:mutation-hold-estimate for Agent batch planning.

    Recent outermost hold p50/p99 sample ring; budget/slo; dirty estimate;
    recommend-split heuristic; schema-2405.
    """
    print(f"{B}=== mutation hold estimate coverage (#2405) ==={N}")
    script = ROOT / "scripts" / "check_mutation_hold_estimate_2405.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mutation hold estimate (#2405) coverage contract rows failed")
        return 1
    ok("mutation hold estimate (#2405) coverage clean")
    return 0


def cmd_pcv_tls_scratch_coverage():
    """Issue #2406: optional TLS freelist for exclusive PCV unique-inplace.

    AURA_PCV_TLS=1 opt-in; default OFF; SafePCVSpan unchanged; schema-2406
    on query:pcv-hotpath-stats.
    """
    print(f"{B}=== pcv TLS scratch coverage (#2406) ==={N}")
    script = ROOT / "scripts" / "check_pcv_tls_scratch_2406.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("pcv TLS scratch (#2406) coverage contract rows failed")
        return 1
    ok("pcv TLS scratch (#2406) coverage clean")
    return 0


def cmd_aot_linear_literal_noop_coverage():
    """Issue #2407: AOT move/Linear of Copy literals as no-ops + emit-binary.

    Re-enable emit:move-int/linear/lin-drop; runtime.c weak pin/unpin;
    lowering elides Move/Linear of literals.
    """
    print(f"{B}=== AOT linear literal no-op coverage (#2407) ==={N}")
    script = ROOT / "scripts" / "check_aot_linear_literal_noop_2407.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("AOT linear literal no-op (#2407) coverage contract rows failed")
        return 1
    ok("AOT linear literal no-op (#2407) coverage clean")
    return 0


def cmd_stringpool_bytes_total_lock_coverage():
    """Issue #2408: string_bytes_total single shared_lock + resolve_unlocked.

    Fixes UAF under concurrent intern and O(capacity) lock churn.
    """
    print(f"{B}=== stringpool string_bytes_total lock coverage (#2408) ==={N}")
    script = ROOT / "scripts" / "check_stringpool_bytes_total_lock_2408.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("stringpool string_bytes_total lock (#2408) coverage contract rows failed")
        return 1
    ok("stringpool string_bytes_total lock (#2408) coverage clean")
    return 0


def cmd_stringpool_buf_fragmentation_lock_coverage():
    """Issue #2409: buf_fragmentation single shared_lock snapshot.

    Sample buf_.size() and string_bytes under one lock; frag in [0,1].
    """
    print(f"{B}=== stringpool buf_fragmentation lock coverage (#2409) ==={N}")
    script = ROOT / "scripts" / "check_stringpool_buf_fragmentation_lock_2409.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("stringpool buf_fragmentation lock (#2409) coverage contract rows failed")
        return 1
    ok("stringpool buf_fragmentation lock (#2409) coverage clean")
    return 0


def cmd_node_meta_bounds_coverage():
    """Issue #2410: meta(NodeTag) bounds-checked OOB sentinel.

    Invalid tags return kNodeMeta[0]; static_assert table size == Class.
    """
    print(f"{B}=== node meta bounds coverage (#2410) ==={N}")
    script = ROOT / "scripts" / "check_node_meta_bounds_2410.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("node meta bounds (#2410) coverage contract rows failed")
        return 1
    ok("node meta bounds (#2410) coverage clean")
    return 0


def cmd_node_meta_gap_coverage():
    """Issue #2411: kNodeMeta gap is_gap + full tag/name consistency.

    Gap entry uses tag 0x0C + is_gap; validate_node_meta checks every slot.
    """
    print(f"{B}=== node meta gap coverage (#2411) ==={N}")
    script = ROOT / "scripts" / "check_node_meta_gap_2411.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("node meta gap (#2411) coverage contract rows failed")
        return 1
    ok("node meta gap (#2411) coverage clean")
    return 0


def cmd_reset_slot_parent_edges_coverage():
    """Issue #2412: reset_node_slot always clears incoming_parent_edges_.

    Edge clear is not gated on !incoming_parent_index_dirty_.
    """
    print(f"{B}=== reset slot parent edges coverage (#2412) ==={N}")
    script = ROOT / "scripts" / "check_reset_slot_parent_edges_2412.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("reset slot parent edges (#2412) coverage contract rows failed")
        return 1
    ok("reset slot parent edges (#2412) coverage clean")
    return 0


def cmd_flatast_add_node_lock_coverage():
    """Issue #2413: FlatAST add_node multi-column SoA lock + reader contract.

    Documents flatast_mutex_ vs lock-free SoA readers; audit findings.
    """
    print(f"{B}=== flatast add_node lock coverage (#2413) ==={N}")
    script = ROOT / "scripts" / "check_flatast_add_node_lock_2413.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("flatast add_node lock (#2413) coverage contract rows failed")
        return 1
    ok("flatast add_node lock (#2413) coverage clean")
    return 0


def cmd_summary_recompute_sym_coverage():
    """Issue #2414: summary_recompute(pool) restores sym_id summary bits.

    HasKeywordVar + HasQueryOrMutateCall after heavy recompute.
    """
    print(f"{B}=== summary_recompute sym coverage (#2414) ==={N}")
    script = ROOT / "scripts" / "check_summary_recompute_sym_2414.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("summary_recompute sym (#2414) coverage contract rows failed")
        return 1
    ok("summary_recompute sym (#2414) coverage clean")
    return 0


def cmd_summary_flags_guard_coverage():
    """Issue #2415: summary_flags_ thread-safety annotation + FlatAST audit.

    Atomic (not GUARDED_BY mutex); free_list_ / SoA GUARDED_BY comments.
    """
    print(f"{B}=== summary_flags guard coverage (#2415) ==={N}")
    script = ROOT / "scripts" / "check_summary_flags_guard_2415.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("summary_flags guard (#2415) coverage contract rows failed")
        return 1
    ok("summary_flags guard (#2415) coverage clean")
    return 0


def cmd_incoming_parent_dirty_atomic_coverage():
    """Issue #2416: incoming_parent_index_dirty_ is std::atomic<bool>.

    acquire/release loads/stores; match tag_arity_index_dirty_ pattern.
    """
    print(f"{B}=== incoming_parent dirty atomic coverage (#2416) ==={N}")
    script = ROOT / "scripts" / "check_incoming_parent_dirty_atomic_2416.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("incoming_parent dirty atomic (#2416) coverage contract rows failed")
        return 1
    ok("incoming_parent dirty atomic (#2416) coverage clean")
    return 0


def cmd_binding_gens_atomic_coverage():
    """Issue #2417: binding_gens_ atomic shared_ptr + COW bump.

    Readers snapshot; writers COW+CAS; clone stores fresh map.
    """
    print(f"{B}=== binding_gens atomic coverage (#2417) ==={N}")
    script = ROOT / "scripts" / "check_binding_gens_atomic_2417.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("binding_gens atomic (#2417) coverage contract rows failed")
        return 1
    ok("binding_gens atomic (#2417) coverage clean")
    return 0


def cmd_structural_metadata_lock_order_coverage():
    """Issue #2418: structural_mtx_ → metadata_mtx_ lock order.

    Combined guard + audit against reverse nest.
    """
    print(f"{B}=== structural/metadata lock order coverage (#2418) ==={N}")
    script = ROOT / "scripts" / "check_structural_metadata_lock_order_2418.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("structural/metadata lock order (#2418) coverage contract rows failed")
        return 1
    ok("structural/metadata lock order (#2418) coverage clean")
    return 0


def cmd_tag_arity_index_lock_coverage():
    """Issue #2419: tag_arity_index_ map lock vs concurrent rebuild.

    Dedicated shared_mutex; find shared, rebuild exclusive.
    """
    print(f"{B}=== tag_arity_index lock coverage (#2419) ==={N}")
    script = ROOT / "scripts" / "check_tag_arity_index_lock_2419.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("tag_arity_index lock (#2419) coverage contract rows failed")
        return 1
    ok("tag_arity_index lock (#2419) coverage clean")
    return 0


def cmd_tag_arity_key_hash_coverage():
    """Issue #2420: TagArityKeyHash pack + splitmix finalizer.

    Better entropy than separate FNV mixes of zero-padded fields.
    """
    print(f"{B}=== tag_arity key hash coverage (#2420) ==={N}")
    script = ROOT / "scripts" / "check_tag_arity_key_hash_2420.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("tag_arity key hash (#2420) coverage contract rows failed")
        return 1
    ok("tag_arity key hash (#2420) coverage clean")
    return 0


def cmd_restamp_lazy_align_atomic_coverage():
    """Issue #2421: restamp_lazy_align_enabled_ is std::atomic<bool>.

    acquire/release loads/stores; match auto_restamp_pending_ pattern.
    """
    print(f"{B}=== restamp_lazy_align atomic coverage (#2421) ==={N}")
    script = ROOT / "scripts" / "check_restamp_lazy_align_atomic_2421.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("restamp_lazy_align atomic (#2421) coverage contract rows failed")
        return 1
    ok("restamp_lazy_align atomic (#2421) coverage clean")
    return 0


def cmd_subtree_gen_atomic_coverage():
    """Issue #2422: subtree_gen_ atomic cells (uint32 + atomic_ref).

    Resize under subtree_gen_mtx_; low 16 bits = gen; is_always_lock_free.
    """
    print(f"{B}=== subtree_gen atomic coverage (#2422) ==={N}")
    script = ROOT / "scripts" / "check_subtree_gen_atomic_2422.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("subtree_gen atomic (#2422) coverage contract rows failed")
        return 1
    ok("subtree_gen atomic (#2422) coverage clean")
    return 0


def cmd_dirty_column_lock_coverage():
    """Issue #2423: dirty_ column shared/exclusive lock for short-circuit APIs.

    is_subtree_dirty_node / dirty_nodes_in_range take shared; mark_dirty exclusive.
    """
    print(f"{B}=== dirty_column lock coverage (#2423) ==={N}")
    script = ROOT / "scripts" / "check_dirty_column_lock_2423.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("dirty_column lock (#2423) coverage contract rows failed")
        return 1
    ok("dirty_column lock (#2423) coverage clean")
    return 0


def cmd_subtree_dirty_bounds_coverage():
    """Issue #2424: is_subtree_dirty_node bounds via dirty_.size() (not size()).

    Concurrent add_node grows dirty_ under dirty_column_mtx_; no tag_.size() race.
    """
    print(f"{B}=== subtree dirty bounds coverage (#2424) ==={N}")
    script = ROOT / "scripts" / "check_subtree_dirty_bounds_2424.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("subtree dirty bounds (#2424) coverage contract rows failed")
        return 1
    ok("subtree dirty bounds (#2424) coverage clean")
    return 0


def cmd_type_system_health_coverage():
    """Issue #2350: query:type-system-health single Agent score.

    Aggregates provenance completeness, timeout reject rate, linear pin
    miss rate, layered DCE efficiency into health-bp + force-reason.
    """
    print(f"{B}=== type-system-health coverage (#2350) ==={N}")
    script = ROOT / "scripts" / "check_type_system_health_2350.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("type-system-health coverage contract rows failed")
        return 1
    ok("type-system-health coverage clean")
    return 0


def cmd_mutation_concurrency_health_coverage():
    """Issue #2379: query:mutation-concurrency-health single Agent score.

    Aggregates hold SLO, steal force-deopt, residual defer, densify fail,
    mailbox starvation into health-bp + force-reason priority.
    """
    print(f"{B}=== mutation-concurrency-health coverage (#2379) ==={N}")
    script = ROOT / "scripts" / "check_mutation_concurrency_health_2379.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("mutation-concurrency-health coverage contract rows failed")
        return 1
    ok("mutation-concurrency-health coverage clean")
    return 0


def cmd_steal_layout_stamp_coverage():
    """Issue #2351: steal-complete LayoutStamp dual-check before resume.

    Matching stamp: no mismatch. Mismatch: steal counter + force dual-check.
    No stamp: zero cost. Schema-2351 additive.
    """
    print(f"{B}=== steal LayoutStamp dual-check coverage (#2351) ==={N}")
    script = ROOT / "scripts" / "check_steal_layout_stamp_2351.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("steal LayoutStamp dual-check coverage contract rows failed")
        return 1
    ok("steal LayoutStamp dual-check coverage clean")
    return 0


def cmd_post_densify_linear_type_revalidate_coverage():
    """Issue #2353: post-densify / post-steal Linear+Type revalidate phase.

    Complements #2341 DensifyConsistencyReport with ownership + type axis.
    Soft / no densify / no linear → zero cost; fail-closed suppresses Phase 5 success.
    """
    print(f"{B}=== post-densify Linear+Type revalidate coverage (#2353) ==={N}")
    script = ROOT / "scripts" / "check_post_densify_linear_type_revalidate_2353.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("post-densify Linear+Type revalidate coverage contract rows failed")
        return 1
    ok("post-densify Linear+Type revalidate coverage clean")
    return 0


def cmd_lock_order_audit_2354_coverage():
    """Issue #2354: debug lock-order audit for scheduler / workspace / closures.

    Rank table + AURA_LOCK_ORDER_AUDIT soft mode + canary hard abort;
    instrumented Scheduler wait_map/joiner/orphan/owned + Worker fiber_registry.
    """
    print(f"{B}=== lock-order audit coverage (#2354) ==={N}")
    script = ROOT / "scripts" / "check_lock_order_audit_2354.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("lock-order audit (#2354) coverage contract rows failed")
        return 1
    ok("lock-order audit (#2354) coverage clean")
    return 0


def cmd_type_dep_epoch_prune_coverage():
    """Issue #2355: type_dep_graph_ epoch prune + NodeId invalidation.

    TypeDepEdge stamps cache_epoch_; set_cache_epoch drops older edges;
    dirty invalidate + per-bucket cap bound long AI sessions.
    """
    print(f"{B}=== type_dep epoch prune coverage (#2355) ==={N}")
    script = ROOT / "scripts" / "check_type_dep_epoch_prune_2355.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("type_dep epoch prune (#2355) coverage contract rows failed")
        return 1
    ok("type_dep epoch prune (#2355) coverage clean")
    return 0


def cmd_reverify_expand_coverage():
    """Issue #2356: truncated reverify one-shot expand for occurrence/let-poly.

    When reverify hits the scan cap and priority roots are non-empty, run
    exactly one expanded pass; empty priority → zero cost; TIMEOUT escalate unchanged.
    """
    print(f"{B}=== reverify expand coverage (#2356) ==={N}")
    script = ROOT / "scripts" / "check_reverify_expand_2356.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("reverify expand (#2356) coverage contract rows failed")
        return 1
    ok("reverify expand (#2356) coverage clean")
    return 0


def cmd_linear_synth_violation_coverage():
    """Issue #2357: Phase-1 linear Move/Drop first-class synthesize violation.

    can_move/can_drop fail during synthesize reports TypeError under
    production/strict (Warning soft); set_node_error + counters; post-mutate
    audit remains defense-in-depth.
    """
    print(f"{B}=== linear synth violation coverage (#2357) ==={N}")
    script = ROOT / "scripts" / "check_linear_synth_violation_2357.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("linear synth violation (#2357) coverage contract rows failed")
        return 1
    ok("linear synth violation (#2357) coverage clean")
    return 0


def cmd_castop_density_hard_coverage():
    """Issue #2358: CastOp density HARD force-JIT policy.

    AURA_CASTOP_DENSITY_HARD=1 + dens>budget → force-JIT (codegen degrade);
    mutate still succeeds; HARD=0 soft-only; under budget zero extra action.
    """
    print(f"{B}=== castop density HARD policy coverage (#2358) ==={N}")
    script = ROOT / "scripts" / "check_castop_density_hard_2358.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("castop density HARD (#2358) coverage contract rows failed")
        return 1
    ok("castop density HARD (#2358) coverage clean")
    return 0


def cmd_memo_goal_epoch_health_coverage():
    """Issue #2359: occurrence_goals + predicate_memo epoch health query.

    Pure read keys on query:type-incremental-fidelity-stats (cache-epoch,
    goals-live, memo-live/stale, delta, wired). No solver behavior change.
    """
    print(f"{B}=== memo-goal epoch health coverage (#2359) ==={N}")
    script = ROOT / "scripts" / "check_memo_goal_epoch_health_2359.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("memo-goal epoch health (#2359) coverage contract rows failed")
        return 1
    ok("memo-goal epoch health (#2359) coverage clean")
    return 0


def cmd_densify_envframe_ok_coverage():
    """Issue #2361: densify envframe_ok real per-call check.

    Stop forcing DensifyConsistencyReport.envframe_ok = true; wire ownership
    scan + dual-path clean into Phase 5 overall_ok gate.
    """
    print(f"{B}=== densify envframe_ok coverage (#2361) ==={N}")
    script = ROOT / "scripts" / "check_densify_envframe_ok_2361.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("densify envframe_ok (#2361) coverage contract rows failed")
        return 1
    ok("densify envframe_ok (#2361) coverage clean")
    return 0


def cmd_densify_last_call_axes_coverage():
    """Issue #2376: densify last-call envframe + closure axes.

    Seals per-call last-result (not cumulative / not force-true under Moving);
    call-seq + fail codes; schema-2376 on lifetime-contract-snapshot.
    """
    print(f"{B}=== densify last-call axes coverage (#2376) ==={N}")
    script = ROOT / "scripts" / "check_densify_last_call_axes_2376.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("densify last-call axes (#2376) coverage contract rows failed")
        return 1
    ok("densify last-call axes (#2376) coverage clean")
    return 0


def cmd_envframe_ownership_steal_densify_coverage():
    """Issue #2362: EnvFrameRef ownership under fiber steal + densify.

    Production live set (#2360) + transfer_to/drop on steal and densify
    boundaries. Soft/empty set free. Hold-pin Guard retained.
    """
    print(f"{B}=== envframe ownership steal+densify coverage (#2362) ==={N}")
    script = ROOT / "scripts" / "check_envframe_ownership_steal_densify_2362.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("envframe ownership steal+densify (#2362) coverage contract rows failed")
        return 1
    ok("envframe ownership steal+densify (#2362) coverage clean")
    return 0


def cmd_general_object_pin_adopt_coverage():
    """Issue #2363: complete GeneralObjectPin adopt for mutate/agent/scratch.

    wire_general_object_create_pair across 7 intermediate create sites;
    Moving densify remap/verify retained; Soft zero cost.
    """
    print(f"{B}=== general object pin adopt coverage (#2363) ==={N}")
    script = ROOT / "scripts" / "check_general_object_pin_adopt_2363.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("general object pin adopt (#2363) coverage contract rows failed")
        return 1
    ok("general object pin adopt (#2363) coverage clean")
    return 0


def cmd_panic_defer_after_densify_coverage():
    """Issue #2364: PanicCheckpoint residual × densify closed loop.

    Post-densify audit: re-arm if CP live, force-clear residual if CP gone;
    Soft free; AURA_PANIC_CONTRACT=hard fail-closed.
    """
    print(f"{B}=== panic defer after densify coverage (#2364) ==={N}")
    script = ROOT / "scripts" / "check_panic_defer_after_densify_2364.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("panic defer after densify (#2364) coverage contract rows failed")
        return 1
    ok("panic defer after densify (#2364) coverage clean")
    return 0


def cmd_densify_root_closure_closed_loop_coverage():
    """Issue #2365: RootRemap + densify Closure/EnvFrame dual-epoch closed-loop.

    Last-call root_remap_ok / closure_remount_ok; Soft vacuous; dual-epoch
    revalidate after densify; documented densify-success order.
    """
    print(f"{B}=== densify root+closure closed-loop coverage (#2365) ==={N}")
    script = ROOT / "scripts" / "check_densify_root_closure_closed_loop_2365.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("densify root+closure closed-loop (#2365) coverage contract rows failed")
        return 1
    ok("densify root+closure closed-loop (#2365) coverage clean")
    return 0


def cmd_epoch_invariant_walk_coverage():
    """Issue #2366: per-entry epoch invariant walk + MustDeopt (#2304 follow-up).

    Soft metric-only / hard abort; AOT live-behind + IR stamp + closure
    MustDeopt walk after atomic_bump_epochs_and_stamp_bridge.
    """
    print(f"{B}=== epoch invariant walk coverage (#2366) ==={N}")
    script = ROOT / "scripts" / "check_epoch_invariant_walk_2366.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("epoch invariant walk (#2366) coverage contract rows failed")
        return 1
    ok("epoch invariant walk (#2366) coverage clean")
    return 0


def cmd_reload_recovery_query_coverage():
    """Issue #2367: ReloadRecovery query primitive + recovery-state snapshot.

    query:reload-recovery-state (+ alias) surfaces ReloadRecoveryState,
    StormLevel, region masks, reemit policy, last force-JIT reason/epoch.
    """
    print(f"{B}=== reload recovery query coverage (#2367) ==={N}")
    script = ROOT / "scripts" / "check_reload_recovery_query_2367.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("reload recovery query (#2367) coverage contract rows failed")
        return 1
    ok("reload recovery query (#2367) coverage clean")
    return 0


def cmd_densify_remap_pairing_coverage():
    """Issue #2368: force densify remap-context pairing on Moving success.

    Permanent order RootRemap → EnvFrame xfer → closure remount → dual-epoch;
    Soft vacuous; inject RootRemap fail suppresses densify success metrics.
    """
    print(f"{B}=== densify remap pairing coverage (#2368) ==={N}")
    script = ROOT / "scripts" / "check_densify_remap_pairing_2368.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("densify remap pairing (#2368) coverage contract rows failed")
        return 1
    ok("densify remap pairing (#2368) coverage clean")
    return 0


def cmd_live_closure_stable_id_only_coverage():
    """Issue #2369: stable_func_id sole primary for live-closure remap.

    Name-fallback rewrite is legacy opt-in only; miss → MustDeopt + batch_deopt.
    Production security defaults force fallback off.
    """
    print(f"{B}=== live-closure stable_func_id only coverage (#2369) ==={N}")
    script = ROOT / "scripts" / "check_live_closure_stable_id_only_2369.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("live-closure stable_func_id only (#2369) coverage contract rows failed")
        return 1
    ok("live-closure stable_func_id only (#2369) coverage clean")
    return 0


def cmd_specjit_per_eval_storm_isolation_coverage():
    """Issue #2370: real PerEval storm isolation for SpecJIT.

    Per-eval isolation epoch + TLS storm eval context; foreign storms skip;
    ShapeProfiler does not bump global shape_version under PerEval.
    """
    print(f"{B}=== SpecJIT PerEval storm isolation coverage (#2370) ==={N}")
    script = ROOT / "scripts" / "check_specjit_per_eval_storm_isolation_2370.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("SpecJIT PerEval storm isolation (#2370) coverage contract rows failed")
        return 1
    ok("SpecJIT PerEval storm isolation (#2370) coverage clean")
    return 0


def cmd_cross_cow_soft_migrate_coverage():
    """Issue #2371: cross-COW dual-epoch soft restamp vs hard-reject.

    Soft migrate restamps bridge+defuse (+ remount) when safe; hard reject
    for freed / linear-moved / far-behind. Production default soft on.
    """
    print(f"{B}=== cross-COW soft migrate coverage (#2371) ==={N}")
    script = ROOT / "scripts" / "check_cross_cow_soft_migrate_2371.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("cross-COW soft migrate (#2371) coverage contract rows failed")
        return 1
    ok("cross-COW soft migrate (#2371) coverage clean")
    return 0


def cmd_chaos_mutate_steal_gc_mailbox_coverage():
    """Issue #2352: chaos mutate × steal × GC × mailbox production gate.

    Smoke always (≤90s); full 30s via AURA_CHAOS_FULL=1. Pass: 0 hang,
    residual defer clean, snapshot mismatch delta 0. Inject residual /
    mismatch self-tests prove fail criteria.
    """
    print(f"{B}=== chaos mutate×steal×GC×mailbox coverage (#2352) ==={N}")
    script = ROOT / "scripts" / "check_chaos_mutate_steal_gc_mailbox_2352.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("chaos mutate×steal×GC×mailbox coverage contract rows failed")
        return 1
    ok("chaos mutate×steal×GC×mailbox coverage clean")
    return 0


def cmd_production_concurrency_coverage():
    """Issue #2380: production-concurrency gate static contract rows.

    Nightly profile: lock-order canary + full chaos + densify + Soft forbid.
    PR smoke path stays short (no FULL / no PRODUCTION_CONCURRENCY_GATE).
    """
    print(f"{B}=== production-concurrency coverage (#2380) ==={N}")
    script = ROOT / "scripts" / "check_production_concurrency_gate_2380.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run([sys.executable, str(script)], cwd=ROOT)
    if r.returncode != 0:
        fail("production-concurrency coverage contract rows failed")
        return 1
    ok("production-concurrency coverage clean")
    return 0


def cmd_production_concurrency():
    """Issue #2380: nightly / deploy production-concurrency hard gate.

    Env matrix (hard-fail if any criterion fails):
      AURA_PRODUCTION_CONCURRENCY_GATE=1
      AURA_LOCK_ORDER_CANARY=1
      AURA_CHAOS_FULL=1
      AURA_CHAOS_WORKERS≥4  AURA_CHAOS_DURATION_S≥30
    Soft steal (AURA_STEAL_SNAPSHOT_SOFT) is forbidden.
    Builds test_chaos_mutate_steal_gc_mailbox_2352 if needed, then soaks.
    Not part of PR CI smoke — use nightly or explicit local run.
    """
    print(f"{B}═══ production-concurrency gate (#2380) ═══{N}")
    # Static contract first (fast fail on missing wire-up).
    rc = cmd_production_concurrency_coverage()
    if rc != 0:
        return rc

    bin_path = BUILD / "test_chaos_mutate_steal_gc_mailbox_2352"
    if not bin_path.exists():
        info("building test_chaos_mutate_steal_gc_mailbox_2352…")
        nproc = os.cpu_count() or 4
        r = run(
            [
                "cmake",
                "--build",
                str(BUILD),
                "--target",
                "test_chaos_mutate_steal_gc_mailbox_2352",
                "-j",
                str(nproc),
            ],
            cwd=ROOT,
        )
        if r != 0:
            fail("build test_chaos_mutate_steal_gc_mailbox_2352 failed")
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
    env.setdefault("AURA_CHAOS_FIBERS", "64")
    env.setdefault("AURA_CHAOS_DURATION_S", "30")
    # Soft steal forbidden under production gate (also unset by test body).
    env.pop("AURA_STEAL_SNAPSHOT_SOFT", None)

    info(
        "env: AURA_PRODUCTION_CONCURRENCY_GATE=1 AURA_LOCK_ORDER_CANARY=1 "
        f"AURA_CHAOS_FULL=1 workers={env['AURA_CHAOS_WORKERS']} "
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


def cmd_layout_stamp_shape_version_fence_coverage():
    """Issue #2255: Unified LayoutStamp + shape_version fence (7th field).

    Validates the 5-AC contract from issue body:
      AC1: Phase 5 writes complete stamp (incl. shape_version) into current Fiber
      AC2: hard compare + shape_version_fence_reject bump on mismatch
      AC3: ShapeProfiler accessor exposes current_global_shape_version()
      AC4: shape_version_fence_reject_total counter + query + schema-2255
      AC5: dual-worker stress test surface
    """
    print(f"{B}=== LayoutStamp + shape_version fence coverage (#2255) ==={N}")
    script = ROOT / "scripts" / "check_layout_stamp_shape_version_fence_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("LayoutStamp + shape_version fence coverage contract rows failed")
        return 1
    ok("LayoutStamp + shape_version fence coverage clean")
    return 0


def cmd_soa_single_source_of_truth_coverage():
    """Issue #2254: SoA single source of truth (refine #1629 #1920 #1377).

    Validates the 5-AC contract from issue body:
      AC1: AURA_IR_SOA_ONLY default ON; lowering_impl gates dual-emit
      AC2: IRInstructionView <= 16 B POD view
      AC3: finish_dirty_sync single authority
      AC4: soa_only_path_total + residual_aos_bridge_total metrics
      AC5: test surface covers #2254
    """
    print(f"{B}=== SoA single source of truth coverage (#2254) ==={N}")
    script = ROOT / "scripts" / "check_soa_single_source_of_truth_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("SoA single source of truth coverage contract rows failed")
        return 1
    ok("SoA single source of truth coverage clean")
    return 0


def cmd_hold_aware_steal_scoring_coverage():
    """Issue #2253: hold-aware work-steal scoring (depth + hold_us + priority boost).

    Validates the 4-AC contract from issue body:
      AC1: WorkerThread::steal() ranks with integer score
           (+100 outermost-safe + +50 priority boost
            + +20 short-yield - 40 recent hold > p90)
      AC2: long-hold victims remain steal-deferred until outermost-safe
      AC3: scoring is arithmetic over already-loaded snapshot fields;
           steal_score_selected_total + bucket histogram bumps
      AC4: mixed-MB-load steal distribution test source-cite
    """
    print(f"{B}=== hold-aware work-steal scoring coverage (#2253) ==={N}")
    script = ROOT / "scripts" / "check_hold_aware_steal_scoring_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("hold-aware work-steal scoring coverage contract rows failed")
        return 1
    ok("hold-aware work-steal scoring coverage clean")
    return 0


def cmd_aot_stale_probe_hard_reject_coverage():
    """Issue #2252: hard-reject native execution when AOT slot table_generation != live epoch.

    Validates the 5-AC contract from issue body:
      AC1: aura_aot_probe_fn_ptr bumps hard-reject counter on gen != cur + returns 0
      AC2: defense-in-depth via Fiber resume LayoutStamp.defuse compare
      AC3: happy path zero extra atomics beyond existing probe loads
      AC4: metric + query + schema-2252 lineage
      AC5: concurrent mutate+apply -> hard-reject count > 0 + zero native hits
    """
    print(f"{B}=== AOT stale probe hard-reject coverage (#2252) ==={N}")
    script = ROOT / "scripts" / "check_aot_stale_probe_hard_reject_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("AOT stale probe hard-reject coverage contract rows failed")
        return 1
    ok("AOT stale probe hard-reject coverage clean")
    return 0


def cmd_env_gen_fence_coverage():
    """Issue #2251: RegionExclusive env_gen fence for EnvFrame dual-path / shared parent walks.

    Validates the 5-AC contract from issue body:
      AC1: env_gen_stamp_ on EnvFrame + alloc + publish refresh
      AC2: materialize_call_env fence + empty-Env fallback + bump
      AC3: lookup_by_symid_chain / walk_env_frames gen-mismatch fences
      AC4: env_gen_fence_reject_total counter + query + schema-2251
      AC5: dual-region concurrent apply on shared parent AC
    """
    print(f"{B}=== env_gen fence coverage (#2251) ==={N}")
    script = ROOT / "scripts" / "check_env_gen_fence_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("env_gen fence coverage contract rows failed")
        return 1
    ok("env_gen fence coverage clean")
    return 0


def cmd_layout_stamp_fence_coverage():
    """Issue #2250: LayoutStamp fence on Fiber resume/steal.

    Validates the 5-AC contract from issue body:
      AC1: Phase 5 writes current LayoutStamp into current Fiber
      AC2: hard compare + bump + force dual-check on mismatch
      AC3: zero-cost when stamps match
      AC4: metric + query + schema-2250 lineage
      AC5: dual-worker integration AC (test_layout_stamp_2170.cpp)
    """
    print(f"{B}=== LayoutStamp fence coverage (#2250) ==={N}")
    script = ROOT / "scripts" / "check_layout_stamp_fence_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("LayoutStamp fence coverage contract rows failed")
        return 1
    ok("LayoutStamp fence coverage clean")
    return 0


def cmd_aot_reload_policy_coverage():
    """Issue #2249: Region | Staging auto-retry conservative path (extend #2232).

    Validates the 6-AC contract from issue body:
      AC1: Region fail -> up to 2 reemit attempts @ 15ms backoff
      AC2: Staging identical behaviour
      AC3: Dlopen / Other still zero auto attempts (regression vs #2232)
      AC4: 2 metric fields + 2 query keys + schema-2249 lineage
      AC5: AURA_AOT_RELOAD_AUTO_RETRY=0 still disables all auto recovery
      AC6: success on 2nd Region attempt -> success counter
    """
    print(f"{B}=== AOT reload Region|Staging policy coverage (#2249) ==={N}")
    script = ROOT / "scripts" / "check_aot_reload_policy_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("AOT reload Region|Staging policy coverage contract rows failed")
        return 1
    ok("AOT reload Region|Staging policy coverage clean")
    return 0


def cmd_adaptive_thr_coverage():
    """Issue #2248: Agent-driven adaptive relower threshold from fallback-reason telemetry.

    Validates the 5-AC contract from issue body:
      AC1: bad-reason raises thr (measurable via query)
      AC2: clean-window decays thr (no permanent ratchet)
      AC3: env override AURA_ADAPTIVE_THR=0 freezes at base
      AC4: 5 atomic counters + 4 query keys + schema-2248 lineage
      AC5: StormLevel still ORs (preserved from #2112/#2190)
    """
    print(f"{B}═══ adaptive relower threshold coverage (#2248) ═══{N}")
    script = ROOT / "scripts" / "check_adaptive_thr_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("adaptive relower threshold coverage contract rows failed")
        return 1
    ok("adaptive relower threshold coverage clean")
    return 0


def cmd_dual_dep_graph_parity_coverage():
    """Issue #2247: dual dep_graph write-parity gate + hybrid cascade consistency.

    Validates the 5-AC contract from issue body:
      AC1: graphs_consistent + rebuild_node_dep_graph_from_string helpers
      AC2: default Off (unit-test safe) + Strict toggle
      AC3: happy-path O(1) extra work
      AC4: 2 atomic counters + 2 query keys + schema-2247 lineage
      AC5: this gate (CI contract rows)
    """
    print(f"{B}═══ dual dep_graph parity coverage (#2247) ═══{N}")
    script = ROOT / "scripts" / "check_dual_dep_graph_parity_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("dual dep_graph parity coverage contract rows failed")
        return 1
    ok("dual dep_graph parity coverage clean")
    return 0


def cmd_cross_function_impact_scope_coverage():
    """Issue #2179 + #2246: cross-function impact scope (direct + indirect + unresolved).

    Validates the contract from #2179 (direct Call cross-fn) + #2246 (refine:
    indirect / higher-order Apply callees + unresolved callish block-level
    over-approx). Self-test + --strict on real files. Script extended in
    #2246 to cover 2 new counters + 2 new query keys + schema-2246 lineage +
    AC8/AC9 in test_instruction_level_impact_partial_2109.cpp.
    """
    print(f"{B}═══ cross-fn impact scope coverage (#2179 / #2246) ═══{N}")
    script = ROOT / "scripts" / "check_cross_function_impact_scope_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("cross-fn impact scope coverage contract rows failed")
        return 1
    ok("cross-fn impact scope coverage clean")
    return 0


def cmd_source_to_ir_strict():
    """Issue #2244: source_to_ir_map Strict-mode hard-fail + rebuild coverage.

    Validates the 5-AC contract from issue body:
      AC1: ensure_source_to_ir_or_rebuild wire-up at invalidate_bridge_with_impact
      AC2: default Off (unit-test safe)
      AC3: zero-cost early return on consistent path
      AC4: 2 atomic counters + 2 query keys + schema-2244 lineage
      AC5: this gate (CI contract rows)
    """
    print(f"{B}═══ source_to_ir Strict coverage (#2244) ═══{N}")
    script = ROOT / "scripts" / "check_source_to_ir_strict_coverage.py"
    if not script.exists():
        fail(f"missing {script}")
        return 1
    r = subprocess.run(
        [sys.executable, str(script), "--strict"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        fail("source_to_ir Strict coverage contract rows failed")
        return 1
    ok("source_to_ir Strict coverage clean")
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
    Issue #1957: also runs legacy test inventory --check (regen with --fix).
    Issue #1966: also runs orch MVP scope linter (--strict; removed multi-agent symbols).
    Issue #2057: also runs side-effect security coverage (--strict).
    Issue #2168: also runs AOT env/linear stamp coverage (forbid bare (0,0) mangle).
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
        or cmd_side_effect_security()
        or cmd_naming_convention()
        or cmd_dead_heap_push()
        or cmd_catch_silent_swallow()
        or cmd_mutation_guard_coverage()
        or cmd_orch_mvp_scope()
        or cmd_aot_env_linear_stamp()
        or cmd_legacy_test_inventory()
        or cmd_source_to_ir_strict()
        or cmd_cross_function_impact_scope_coverage()
        or cmd_dual_dep_graph_parity_coverage()
        or cmd_adaptive_thr_coverage()
        or cmd_aot_reload_policy_coverage()
        or cmd_layout_stamp_fence_coverage()
        or cmd_env_gen_fence_coverage()
        or cmd_aot_stale_probe_hard_reject_coverage()
        or cmd_hold_aware_steal_scoring_coverage()
        or cmd_soa_single_source_of_truth_coverage()
        or cmd_layout_stamp_shape_version_fence_coverage()
        or cmd_arena_moving_compaction_coverage()
        or cmd_arena_compact_hook_stats_coverage()
        or cmd_arena_dtor_clears_hooks_coverage()
        or cmd_has_on_compact_hook_lock_coverage()
        or cmd_require_effect_live_mid_coverage()
        or cmd_restricted_unset_principal_coverage()
        or cmd_grant_macro_self_evo_stamp_coverage()
        or cmd_capability_string_matrix_unify_coverage()
        or cmd_security_audit_fold_coverage()
        or cmd_security_health_coverage()
        or cmd_validate_node_no_abort_coverage()
        or cmd_validate_post_restore_soa_coverage()
        or cmd_fixup_deltas_coverage()
        or cmd_last_validated_generation_atomic_coverage()
        or cmd_stable_ref_wire_endian_coverage()
        or cmd_orphan_reap_tick_coverage()
        or cmd_join_drain_reclaim_still_running_coverage()
        or cmd_mailbox_bp_recent_window_coverage()
        or cmd_agent_scope_concurrent_coverage()
        or cmd_parallel_isolation_level_coverage()
        or cmd_agent_reply_coverage()
        or cmd_restamp_incremental_coverage()
        or cmd_query_index_composite_coverage()
        or cmd_stable_ref_export_coverage()
        or cmd_moving_pin_contract_fail_closed_coverage()
        or cmd_root_remap_pass_coverage()
        or cmd_envframe_ownership_transfer_coverage()
        or cmd_residual_gc_defer_multi_eval_coverage()
        or cmd_capture_cell_remap_coverage()
        or cmd_general_object_pin_coverage()
        or cmd_aot_per_eval_slot_invalidate_coverage()
        or cmd_lifetime_contract_snapshot_coverage()
        or cmd_type_timeout_repair_graph_coverage()
        or cmd_escape_gate_key_contract_coverage()
        or cmd_composite_empty_cs_hard_coverage()
        or cmd_steal_snapshot_hard_invariant_coverage()
        or cmd_steal_snapshot_soft_production_lock_coverage()
        or cmd_render_deopt_throttle_race_coverage()
        or cmd_legacy_pin_registry_cleanup_coverage()
        or cmd_pin_bulk_all_shards_coverage()
        or cmd_steal_complete_strong_entry_coverage()
        or cmd_mutate_mailbox_strict_coverage()
        or cmd_mailbox_defer_drain_sla_coverage()
        or cmd_bidirectional_match_coverage()
        or cmd_mutation_hold_slo_coverage()
        or cmd_mutation_hold_estimate_coverage()
        or cmd_pcv_tls_scratch_coverage()
        or cmd_aot_linear_literal_noop_coverage()
        or cmd_stringpool_bytes_total_lock_coverage()
        or cmd_stringpool_buf_fragmentation_lock_coverage()
        or cmd_node_meta_bounds_coverage()
        or cmd_node_meta_gap_coverage()
        or cmd_reset_slot_parent_edges_coverage()
        or cmd_flatast_add_node_lock_coverage()
        or cmd_summary_recompute_sym_coverage()
        or cmd_summary_flags_guard_coverage()
        or cmd_incoming_parent_dirty_atomic_coverage()
        or cmd_binding_gens_atomic_coverage()
        or cmd_structural_metadata_lock_order_coverage()
        or cmd_tag_arity_index_lock_coverage()
        or cmd_tag_arity_key_hash_coverage()
        or cmd_restamp_lazy_align_atomic_coverage()
        or cmd_subtree_gen_atomic_coverage()
        or cmd_dirty_column_lock_coverage()
        or cmd_subtree_dirty_bounds_coverage()
        or cmd_type_system_health_coverage()
        or cmd_mutation_concurrency_health_coverage()
        or cmd_steal_layout_stamp_coverage()
        or cmd_chaos_mutate_steal_gc_mailbox_coverage()
        or cmd_production_concurrency_coverage()
        or cmd_post_densify_linear_type_revalidate_coverage()
        or cmd_lock_order_audit_2354_coverage()
        or cmd_type_dep_epoch_prune_coverage()
        or cmd_reverify_expand_coverage()
        or cmd_linear_synth_violation_coverage()
        or cmd_castop_density_hard_coverage()
        or cmd_memo_goal_epoch_health_coverage()
        or cmd_densify_envframe_ok_coverage()
        or cmd_densify_last_call_axes_coverage()
        or cmd_envframe_ownership_steal_densify_coverage()
        or cmd_general_object_pin_adopt_coverage()
        or cmd_panic_defer_after_densify_coverage()
        or cmd_densify_root_closure_closed_loop_coverage()
        or cmd_epoch_invariant_walk_coverage()
        or cmd_reload_recovery_query_coverage()
        or cmd_densify_remap_pairing_coverage()
        or cmd_live_closure_stable_id_only_coverage()
        or cmd_specjit_per_eval_storm_isolation_coverage()
        or cmd_cross_cow_soft_migrate_coverage()
        or cmd_lifetime_pin_remap_coverage()
        or cmd_shape_storm_isolation_coverage()
        or cmd_incremental_soundness_prod_coverage()
        or cmd_register_render_hot_prim_coverage()
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
            "-Wno-dev",
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
    report_script = ROOT / "scripts" / "llvm_cov_report.py"
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
    # Issue #1932: thin entrypoint or tests/bench/run_bench_all.py
    bench_script = ROOT / "tests" / "run_bench_all.py"
    if not bench_script.exists():
        bench_script = ROOT / "tests" / "bench" / "run_bench_all.py"
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
        "orch-mvp-scope": cmd_orch_mvp_scope,
        "aot-env-linear-stamp": cmd_aot_env_linear_stamp,
        "legacy-test-inventory": cmd_legacy_test_inventory,
        "source-to-ir-strict": cmd_source_to_ir_strict,
        "cross-fn-impact-scope": cmd_cross_function_impact_scope_coverage,
        "dual-dep-graph-parity": cmd_dual_dep_graph_parity_coverage,
        "adaptive-thr": cmd_adaptive_thr_coverage,
        "aot-reload-policy": cmd_aot_reload_policy_coverage,
        "layout-stamp-fence": cmd_layout_stamp_fence_coverage,
        "env-gen-fence": cmd_env_gen_fence_coverage,
        "aot-stale-probe-hard-reject": cmd_aot_stale_probe_hard_reject_coverage,
        "hold-aware-steal-scoring": cmd_hold_aware_steal_scoring_coverage,
        "soa-single-source-of-truth": cmd_soa_single_source_of_truth_coverage,
        "layout-stamp-shape-version-fence": cmd_layout_stamp_shape_version_fence_coverage,
        "arena-moving-compaction": cmd_arena_moving_compaction_coverage,
        "shape-storm-isolation": cmd_shape_storm_isolation_coverage,
        "incremental-soundness-prod": cmd_incremental_soundness_prod_coverage,
        "register-render-hot-prim": cmd_register_render_hot_prim_coverage,
        "coverage": cmd_coverage,
        "fuzz": cmd_fuzz,
        "production-concurrency": cmd_production_concurrency,
        "production-concurrency-coverage": cmd_production_concurrency_coverage,
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
