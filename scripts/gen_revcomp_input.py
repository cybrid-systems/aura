#!/usr/bin/env python3
"""Generate a FASTA DNA input for the reverse-complement benchmark.

The Benchmarks Game's standard test input is a large FASTA file with one
sequence (>header line + 60-char-per-line sequence lines). Aura's stdin
doubles as the program-text stream (see tests/bench/revcomp.aura header),
so this port reads the FASTA from a file path in the INPUT env var instead
of stdin. This script generates that file: random DNA over the full IUB
ambiguous-code alphabet, wrapped 60 chars per line.

Usage:
    python3 scripts/gen_revcomp_input.py 100000 > /tmp/revcomp_input.fasta
    INPUT=/tmp/revcomp_input.fasta ./build-gcc16/aura < tests/bench/revcomp.aura

Argument: number of DNA bases (default 100000).
"""

import random
import sys


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
    rng = random.Random(20260710)
    # Full IUB alphabet (ambiguous codes) — exercises every complement-table arm.
    codes = "ACGTMRWSYKVHDBN"
    seq = "".join(rng.choice(codes) for _ in range(n))
    out = [">revcomp"]
    for i in range(0, len(seq), 60):
        out.append(seq[i : i + 60])
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
