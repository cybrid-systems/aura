#!/usr/bin/env python3
"""Issue #675: reproducible build verification.

Builds the aura Release binary twice with identical hermetic flags
(SOURCE_DATE_EPOCH, prefix maps, fixed random seed) and compares
stripped binary SHA-256 digests.

Usage:
  python3 scripts/ci_reproducibility.py [--epoch EPOCH] [--jobs N]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EPOCH = "1704067200"  # 2024-01-01T00:00:00Z


def _run(cmd: list[str], *, env: dict[str, str] | None = None, cwd: Path = ROOT) -> None:
    print(f"  $ {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=cwd, env=env, check=False)
    if r.returncode != 0:
        raise SystemExit(r.returncode)


def _repro_cmake_flags(src: Path) -> tuple[str, str, str]:
    src_s = str(src)
    cflags = f"-ffile-prefix-map={src_s}=. -fdebug-prefix-map={src_s}=. -frandom-seed=aura-repro-675 -g0"
    ldflags = "-Wl,--build-id=none"
    return cflags, cflags, ldflags


def _configure(build_dir: Path, *, epoch: str, jobs: int) -> None:
    src = ROOT
    cflags, cxxflags, ldflags = _repro_cmake_flags(src)
    env = {**os.environ, "SOURCE_DATE_EPOCH": epoch, "CCACHE_DISABLE": "1"}
    _run(
        [
            "cmake",
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            "-Wno-dev",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_FLAGS={cflags}",
            f"-DCMAKE_CXX_FLAGS={cxxflags}",
            f"-DCMAKE_EXE_LINKER_FLAGS={ldflags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={ldflags}",
        ],
        env=env,
    )
    _run(
        ["cmake", "--build", str(build_dir), "--target", "aura", "-j", str(jobs)],
        env=env,
    )


def _strip_copy(binary: Path, out: Path) -> None:
    shutil.copy2(binary, out)
    _run(["strip", "-o", str(out), str(out)])


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify reproducible aura builds")
    parser.add_argument("--epoch", default=os.environ.get("SOURCE_DATE_EPOCH", DEFAULT_EPOCH))
    parser.add_argument(
        "--jobs", type=int, default=int(os.environ.get("AURA_BUILD_JOBS", "0") or 0) or (os.cpu_count() or 4)
    )
    args = parser.parse_args()

    build_a = ROOT / "build_repro_a"
    build_b = ROOT / "build_repro_b"
    for d in (build_a, build_b):
        if d.exists():
            shutil.rmtree(d)

    print("═══ Reproducible build A ═══")
    _configure(build_a, epoch=args.epoch, jobs=args.jobs)

    print("═══ Reproducible build B ═══")
    _configure(build_b, epoch=args.epoch, jobs=args.jobs)

    bin_a = build_a / "aura"
    bin_b = build_b / "aura"
    if not bin_a.is_file() or not bin_b.is_file():
        print("ERROR: aura binary missing after reproducible builds", file=sys.stderr)
        return 1

    stripped_a = ROOT / ".repro_aura_a.stripped"
    stripped_b = ROOT / ".repro_aura_b.stripped"
    _strip_copy(bin_a, stripped_a)
    _strip_copy(bin_b, stripped_b)

    digest_a = _sha256(stripped_a)
    digest_b = _sha256(stripped_b)

    print(f"  stripped SHA-256 A: {digest_a}")
    print(f"  stripped SHA-256 B: {digest_b}")

    stripped_a.unlink(missing_ok=True)
    stripped_b.unlink(missing_ok=True)

    if digest_a != digest_b:
        print("FAIL: reproducible builds differ (stripped binary mismatch)", file=sys.stderr)
        return 1

    print("PASS: reproducible builds match (stripped SHA-256 identical)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
