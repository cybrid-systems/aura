"""Reproducible build / SBOM / security commands (#5246).

The release-facing commands: dual-build bit-identity, SBOM emit and the
security posture summary. They only need the shared paths, colours and
process runner plus the toolchain helpers in core.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import sys
from pathlib import Path

from tools.build.core import (
    ROOT,
    TOOLS,
    B,
    N,
    _build_jobs,
    _select_fast_linker,
    fail,
    ok,
    run,
    warn,
)

REPRO_SOURCE_DATE_EPOCH = "1704067200"  # 2024-01-01T00:00:00Z

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
