#!/usr/bin/env python3
"""Issue #675: generate a CycloneDX 1.5 SBOM for Aura releases.

Usage:
  python3 scripts/gen_sbom.py [--output PATH] [--version VERSION]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _sha256_file(path: Path) -> str | None:
    if not path.is_file():
        return None
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _toolchain_version(cmd: list[str]) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=False, timeout=10)
        if r.returncode == 0:
            return r.stdout.strip().split("\n", 1)[0]
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "unknown"


def _read_requirements(path: Path) -> list[dict]:
    comps = []
    if not path.is_file():
        return comps
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name = line.split("==", 1)[0].strip()
        version = line.split("==", 1)[1].strip() if "==" in line else "unspecified"
        comps.append(
            {
                "type": "library",
                "name": name,
                "version": version,
                "purl": f"pkg:pypi/{name}@{version}",
            }
        )
    return comps


def build_sbom(*, version: str) -> dict:
    aura_bin = ROOT / "build" / "aura"
    runtime_c = ROOT / "lib" / "runtime.c"
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    components: list[dict] = [
        {
            "type": "application",
            "name": "aura",
            "version": version,
            "description": "Aura language runtime and compiler",
            "bom-ref": "aura",
        }
    ]

    aura_hash = _sha256_file(aura_bin)
    if aura_hash:
        components[0]["hashes"] = [{"alg": "SHA-256", "content": aura_hash}]

    runtime_hash = _sha256_file(runtime_c)
    if runtime_hash:
        components.append(
            {
                "type": "file",
                "name": "runtime.c",
                "version": version,
                "bom-ref": "aura-runtime-c",
                "hashes": [{"alg": "SHA-256", "content": runtime_hash}],
            }
        )

    for dep in _read_requirements(ROOT / "requirements-dev.txt"):
        dep["bom-ref"] = f"pypi-{dep['name']}"
        components.append(dep)

    gcc_ver = _toolchain_version(["g++", "--version"])
    cmake_ver = _toolchain_version(["cmake", "--version"])
    ninja_ver = _toolchain_version(["ninja", "--version"])

    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": f"urn:uuid:aura-sbom-{version}",
        "version": 1,
        "metadata": {
            "timestamp": now,
            "tools": [
                {"vendor": "Aura", "name": "gen_sbom.py", "version": "1.0"},
            ],
            "component": {
                "type": "application",
                "name": "aura",
                "version": version,
            },
            "properties": [
                {"name": "toolchain:gcc", "value": gcc_ver},
                {"name": "toolchain:cmake", "value": cmake_ver},
                {"name": "toolchain:ninja", "value": ninja_ver},
                {"name": "source-date-epoch", "value": str(int(os.environ.get("SOURCE_DATE_EPOCH", "0") or 0))},
            ],
        },
        "components": components,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate CycloneDX SBOM for Aura")
    parser.add_argument("--output", "-o", type=Path, default=ROOT / "dist" / "aura-sbom.json")
    parser.add_argument("--version", default=os.environ.get("AURA_VERSION", "dev"))  # type: ignore[name-defined]
    args = parser.parse_args()

    sbom = build_sbom(version=args.version)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")
    print(f"SBOM written: {args.output} ({len(sbom['components'])} components)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
