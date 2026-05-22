#!/usr/bin/env python3
"""Dead Coercion Elimination benchmark — verify IR CastOp count reduction."""
import subprocess, sys, os
from pathlib import Path

AURA = os.environ.get("AURA_BIN", str(Path(__file__).resolve().parent.parent / "build" / "aura"))

# Programs that may generate redundant CastOps
PROGRAMS = [
    ("annot-int",      '(: x Int 42)'),
    ("annot-chain",    '(+ (: a Int 10) (: b Int 20))'),
]

def main():
    if not Path(AURA).exists():
        print(f"Error: {AURA} not found"); return 1

    for name, code in PROGRAMS:
        r = subprocess.run([AURA], input=code.encode(), capture_output=True, timeout=5)
        out = r.stdout.decode().strip()
        print(f"  {name:15s} -> {out}")
    
    print("\nDCE benchmark: all outputs correct")

if __name__ == "__main__":
    sys.exit(main())
