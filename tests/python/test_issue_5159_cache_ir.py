"""Regression for #5159: aura --cache SIGSEGV in the IR serializer.

The reflect mirror of ClosureBridgeData was shorter than the real type, so
serializing a cache read a std::string out of the shared_ptr control block,
died with SIGSEGV, and left a truncated cache that --cache-open accepted.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AURA = ROOT / "build" / "aura"


def env() -> dict[str, str]:
    e = dict(os.environ)
    for k in ("AURA_SANDBOX", "AURA_PRESET", "AURA_PIPELINE_STRICT", "AURA_KEEP_GOING"):
        e.pop(k, None)
    e["AURA_RUNTIME_DIR"] = str(ROOT)
    return e


def run(args: list[str], src: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(AURA), *args],
        input=src,
        capture_output=True,
        text=True,
        env=env(),
        timeout=60,
        check=False,
    )


def ir_offset(data: bytes) -> int:
    # CacheHeader.ir_offset is the uint64 at file offset 16.
    return int.from_bytes(data[16:24], "little")


class CacheIr5159(unittest.TestCase):
    def test_cache_round_trip_reports_ir_and_refuses_a_truncated_section(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "out.abc"
            wrote = run(["--cache", str(path)], "(display 1)\n")
            self.assertEqual(wrote.returncode, 0, wrote.stderr)
            self.assertNotIn("AURA CRASH", wrote.stderr)
            data = path.read_bytes()
            self.assertGreater(len(data), 72)
            off = ir_offset(data)
            self.assertGreater(off, 0)
            self.assertLessEqual(off + 4, len(data))
            ir_size = int.from_bytes(data[off : off + 4], "little")
            self.assertGreater(ir_size, 0)
            self.assertLessEqual(off + 4 + ir_size, len(data))

            opened = run(["--cache-open", str(path)])
            self.assertEqual(opened.returncode, 0, opened.stderr)
            self.assertIn("IR cache:", opened.stdout)
            self.assertNotIn("AURA CRASH", opened.stderr)
            # "IR cache: N functions, M strings" — N is the lowered function count.
            line = next(ln for ln in opened.stdout.splitlines() if ln.startswith("IR cache:"))
            count = int(line.split()[2])
            self.assertGreater(count, 0)

            # Cut the IR blob after its size prefix. The header still claims
            # the section, so --cache-open must refuse the file.
            path.write_bytes(data[: off + 4 + ir_size // 2])
            refused = run(["--cache-open", str(path)])
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("cannot open cache file", refused.stderr)
            self.assertNotIn("IR cache:", refused.stdout)
            self.assertNotIn("version=3", refused.stdout)

    def test_inspect_ir_prints_the_module(self) -> None:
        r = run(["--inspect", "ir"], "(define (f x) (+ x 1))(display (f 41))\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("AURA CRASH", r.stderr)
        self.assertNotIn("SIGSEGV", r.stderr)
        self.assertIn('"f"', r.stdout)
        self.assertIn("functions", r.stdout)


if __name__ == "__main__":
    unittest.main()
