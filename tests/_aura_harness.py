"""Shared helpers for Aura Python test/build scripts."""

from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
AURA_BIN = BUILD / "aura"

G = "\033[32m"
Y = "\033[33m"
R = "\033[31m"
B = "\033[34m"
C = "\033[36m"
N = "\033[0m"


def ok(msg: str) -> None:
    print(f"  {G}✓{N} {msg}")


def fail(msg: str) -> None:
    print(f"  {R}✗{N} {msg}")


def warn(msg: str) -> None:
    print(f"  {Y}!{N} {msg}")


def info(msg: str) -> None:
    print(f"  {B}→{N} {msg}")


def run(cmd, **kwargs) -> int:
    return subprocess.run(cmd, **kwargs).returncode
