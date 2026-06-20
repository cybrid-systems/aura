#!/usr/bin/env python3
"""
Aura PGO Training Orchestrator

Runs benchmark workloads against the Aura binary to generate
LLVM profile data (default.profraw) for AOT PGO optimization.

Usage:
    # Generate training profiles (run after building with PGO instrumentation)
    python3 tests/pgo_train.py --suite=mixed --iterations=3

    # Default: generate profiles, then merge
    python3 tests/pgo_train.py --suite=mixed --merge
"""

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

_SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = _SCRIPT_DIR.parent  # project root
AURA = os.environ.get("AURA_BIN") or str(ROOT / "build" / "aura")
AURA_ENV = os.environ.copy()

# Default: write profraw to project root (next to build/)
PROFRAW_DIR = ROOT / ".aura-pgo" / "profraw"


def find_llvm_profdata() -> str:
    """Find llvm-profdata binary."""
    candidates = [
        "llvm-profdata",
        "llvm-profdata-20",
        "llvm-profdata-19",
        "llvm-profdata-18",
        "llvm-profdata-17",
    ]
    for c in candidates:
        r = subprocess.run(["which", c], capture_output=True, text=True)
        if r.returncode == 0:
            return c
    # Try LLVM toolchain path from cmake
    import subprocess as sp

    r = sp.run(
        [
            "cmake",
            "--find-package",
            "-DNAME=LLVM",
            "-DCOMPILER_ID=Clang",
            "-DLANGUAGE=C",
            "-DMODE=EXIST",
        ],
        capture_output=True,
        text=True,
    )
    return "llvm-profdata"


def ensure_profraw_dir():
    """Create profraw output directory."""
    PROFRAW_DIR.mkdir(parents=True, exist_ok=True)
    # Clean previous profraw files
    for f in PROFRAW_DIR.glob("*.profraw"):
        f.unlink()


def run_training(suite: str, iterations: int = 3, serve_mode: bool = True):
    """Run training workload and collect profraw."""
    suite_file = ROOT / "tests" / "pgo" / f"suite_{suite.replace('-', '_')}.aura"
    if not suite_file.exists():
        print(f"  ✗ Unknown suite '{suite}' — expected tests/pgo/suite_{suite}.aura")
        sys.exit(1)

    ensure_profraw_dir()

    # Set LLVM profile file pattern so each iteration gets a separate file
    AURA_ENV["LLVM_PROFILE_FILE"] = str(PROFRAW_DIR / "pgo-%p-%m.profraw")

    total_start = time.time()
    for it in range(iterations):
        print(f"  Iteration {it + 1}/{iterations} ... ", end="", flush=True)
        iter_start = time.time()

        if serve_mode:
            # --serve mode: single connection, pipeline inputs
            code = suite_file.read_text()
            p = subprocess.Popen(
                [AURA, "--serve"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                cwd=ROOT,
            )
            out, err = p.communicate(code + "\n", timeout=120)
        else:
            # Direct mode: pipe file content to stdin
            code = suite_file.read_text()
            p = subprocess.run(
                [AURA, "--ir"],  # --ir to bypass type checker
                input=code,
                capture_output=True,
                text=True,
                timeout=120,
                cwd=ROOT,
            )
            _out, err = p.stdout, p.stderr

        elapsed = time.time() - iter_start
        if p.returncode != 0:
            print(f"FAILED (rc={p.returncode})")
            print(f"  stderr: {err[:200]}")
            sys.exit(1)
        print(f"OK ({elapsed:.1f}s)")

    total_elapsed = time.time() - total_start
    print(f"\n  ✓ Training complete: {iterations} iterations in {total_elapsed:.1f}s")

    # Report generated profraw files
    profraw_files = sorted(PROFRAW_DIR.glob("*.profraw"))
    total_size = sum(f.stat().st_size for f in profraw_files)
    print(f"  {len(profraw_files)} profraw file(s), {total_size / 1024:.1f} KB total")
    return profraw_files


def merge_profiles(profraw_files: list, output_dir: Path):
    """Merge profraw files into .profdata."""
    if not profraw_files:
        print("  ✗ No profraw files to merge")
        return None

    profdata_cmd = find_llvm_profdata()
    output_file = output_dir / "aura.profdata"
    profraw_list = list(profraw_files)
    # Also include any profraw in the project root
    for f in ROOT.glob("*.profraw"):
        if f not in profraw_list:
            profraw_list.append(f)

    if not shutil.which(profdata_cmd):
        print(f"  ✗ {profdata_cmd} not found — cannot merge profiles")
        print(f"    LLVM profraw files are at: {PROFRAW_DIR}/")
        return None

    cmd = [profdata_cmd, "merge", "-output", str(output_file)]
    cmd.extend(str(f) for f in profraw_list)
    print(
        f"  Merging {len(profraw_list)} profraw files → {output_file} ... ",
        end="",
        flush=True,
    )
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print("FAILED")
        print(f"  {r.stderr[:300]}")
        return None
    print("OK")
    print(f"  Profile: {output_file} ({output_file.stat().st_size / 1024:.1f} KB)")
    return output_file


def main():
    parser = argparse.ArgumentParser(description="Aura PGO Training")
    parser.add_argument(
        "--suite",
        "-s",
        default="mixed",
        choices=["general", "evo-kv", "mixed"],
        help="Training suite (default: mixed)",
    )
    parser.add_argument(
        "--iterations",
        "-n",
        type=int,
        default=3,
        help="Training iterations (default: 3)",
    )
    parser.add_argument("--merge", "-m", action="store_true", help="Merge profraw after training")
    parser.add_argument(
        "--serve",
        action="store_true",
        default=True,
        help="Use --serve mode (default: true)",
    )
    parser.add_argument("--no-serve", action="store_false", dest="serve", help="Use direct eval mode")

    args = parser.parse_args()

    print(f"{'=' * 55}")
    print(f"  Aura PGO Training: suite={args.suite} iterations={args.iterations}")
    print(f"{'=' * 55}\n")

    profraw_files = run_training(args.suite, args.iterations, args.serve)

    if args.merge and profraw_files:
        output_dir = PROFRAW_DIR.parent  # .aura-pgo/
        profdata = merge_profiles(profraw_files, output_dir)
        if profdata:
            print(f"\n  ✓ PGO profile ready: {profdata}")
            print(f"    Usage: rebuild with -fprofile-instr-use={profdata}")
            return 0

    if profraw_files:
        print(f"\n  ✓ Profraw files at: {PROFRAW_DIR / '*.profraw'}")
        print(f"    Merge with: llvm-profdata merge -output=.aura-pgo/aura.profdata {PROFRAW_DIR}/*.profraw")

    return 0


if __name__ == "__main__":
    sys.exit(main())
