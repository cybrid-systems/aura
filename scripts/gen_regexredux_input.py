#!/usr/bin/env python3
"""Generate a FASTA DNA input for the regex-redux benchmark.

The Benchmarks Game's standard test input is ~10000 bytes of FASTA
produced by the `fasta` program. Aura's stdin doubles as the program-text
stream (see tests/bench/regexredux.aura header), so this port reads the
FASTA from a file path in the INPUT env var instead of stdin. This script
generates that file: random DNA with 70-char line wrap and one >header,
matching the shape the benchmark expects.

Usage:
    python3 scripts/gen_regexredux_input.py 10000 > /tmp/regexredux_input.fasta
    INPUT=/tmp/regexredux_input.fasta ./build-gcc16/aura < tests/bench/regexredux.aura

Argument: number of DNA base characters (default 10000).
"""
import random
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    rng = random.Random(20260709)
    seq = "".join(rng.choice("acgt") for _ in range(n))
    out = [">regexredux"]
    for i in range(0, len(seq), 70):
        out.append(seq[i:i + 70])
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
