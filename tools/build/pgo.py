"""PGO command group (#5246).

Profile-guided optimisation is an independent build pipeline: instrument,
train, merge, optimise.  It only needs the shared paths, colours and process
runner, so it lives here instead of in the build.py CLI table.
"""

from __future__ import annotations

import os
import subprocess
import sys

from tools.build.core import AURA, BUILD, ROOT, B, N, fail, ok, run, warn

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
