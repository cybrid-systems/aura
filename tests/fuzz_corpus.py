#!/usr/bin/env python3
"""Aura Parser Fuzz Corpus — seed-driven fuzz testing.

Usage:
  python3 tests/fuzz_corpus.py                  # run all seeds
  python3 tests/fuzz_corpus.py --quick           # run first 100 seeds
  python3 tests/fuzz_corpus.py --validate-only   # parse-check all seeds, no eval
  python3 tests/fuzz_corpus.py --update          # regenerate seed corpus from stdlib + tests

The seed corpus is in fuzz_seed_corpus/ and contains:
  - stdlib_*.sexpr       — extracted from stdlib .aura files
  - edge_*.sexpr         — AI-generated edge cases (deep nesting, Unicode, etc.)
  - repro_*.sexpr        — from past fuzz reproducers (non-crashing variants)
"""

import hashlib
import os
import subprocess
import sys
import time

AURA = os.environ.get("AURA", "./build/aura")
CORPUS_DIR = os.path.join(os.path.dirname(__file__), "..", "fuzz_seed_corpus")
REPRO_DIR = os.path.join(os.path.dirname(__file__), "reproducers")
PASS = 0
FAIL = 0

def run_seed(path, validate_only=False):
    global PASS, FAIL
    name = os.path.basename(path).removesuffix(".sexpr")
    with open(path) as f:
        code = f.read().strip()
    if not code:
        return

    if validate_only:
        # Parse check: run --ir and check for parse error exit code
        try:
            r = subprocess.run(
                [AURA, "--ir", code],
                capture_output=True, text=True, timeout=10
            )
            if r.returncode == 0:
                PASS += 1
            else:
                print(f"  PARSE FAIL {name}: {r.stderr.strip()[:100]}")
                FAIL += 1
        except subprocess.TimeoutExpired:
            print(f"  TIMEOUT {name}")
            FAIL += 1
        except FileNotFoundError:
            print(f"  ERROR: {AURA} not found")
            sys.exit(1)
    else:
        # Eval: run aura directly, check for crashes (not errors)
        try:
            r = subprocess.run(
                [AURA],
                input=code, capture_output=True, text=True, timeout=10
            )
            if r.returncode >= 128:  # signal = crash
                print(f"  CRASH {name}: returncode={r.returncode}")
                FAIL += 1
            else:
                PASS += 1
        except subprocess.TimeoutExpired:
            print(f"  TIMEOUT {name}")
            FAIL += 1
        except FileNotFoundError:
            print(f"  ERROR: {AURA} not found")
            sys.exit(1)

def main():
    quick = "--quick" in sys.argv
    validate_only = "--validate-only" in sys.argv
    update = "--update" in sys.argv

    if update:
        print("Regenerating seed corpus...")
        # Re-run the generation from stdlib + edge cases
        subprocess.run([sys.executable, "-c", """
import hashlib, os, re

# Extract from stdlib
def extract_sexprs(path):
    with open(path) as f:
        text = f.read()
    sexprs = set()
    for m in re.finditer(r'\\([^)]*\\)', text):
        pass  # skip complex parsing
    depth = 0
    start = -1
    for i, ch in enumerate(text):
        if ch == '(':
            if depth == 0: start = i
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0 and start >= 0:
                sexpr = text[start:i+1]
                if len(sexpr) > 10 and sexpr.count('(') <= 20:
                    sexprs.add(sexpr)
                start = -1
        elif ch == ';':
            if depth == 0: start = -1
    return sexprs

all_sexprs = set()
for root, dirs, files in os.walk('lib/std'):
    for f in files:
        if f.endswith('.aura'):
            all_sexprs.update(extract_sexprs(os.path.join(root, f)))
for root, dirs, files in os.walk('tests/tasks'):
    for f in files:
        if f.endswith('.aura'):
            all_sexprs.update(extract_sexprs(os.path.join(root, f)))

corpus_dir = 'fuzz_seed_corpus'
# Clear old stdlib seeds
for f in os.listdir(corpus_dir):
    if f.startswith('stdlib_') and f.endswith('.sexpr'):
        os.remove(os.path.join(corpus_dir, f))

written = 0
for i, sexpr in enumerate(sorted(all_sexprs)[:500]):
    h = hashlib.sha256(sexpr.encode()).hexdigest()[:12]
    path = os.path.join(corpus_dir, f'stdlib_{i:04d}_{h}.sexpr')
    with open(path, 'w') as f:
        f.write(sexpr + '\\n')
    written += 1
print(f'Generated {written} stdlib seeds')
        """])
        print("Corpus regenerated.")
        return

    # Collect seed files
    seeds = []
    for f in sorted(os.listdir(CORPUS_DIR)):
        if f.endswith(".sexpr"):
            seeds.append(os.path.join(CORPUS_DIR, f))

    if not seeds:
        print(f"ERROR: no .sexpr files in {CORPUS_DIR}")
        print("  Run with --update to generate seeds from stdlib + tasks")
        sys.exit(1)

    if quick:
        seeds = seeds[:100]

    total = len(seeds)
    print(f"Running {total} seeds through fuzz corpus ({'validate-only' if validate_only else 'full eval'})...")

    t0 = time.time()
    for path in seeds:
        run_seed(path, validate_only)
    elapsed = time.time() - t0

    print(f"\nResults: {PASS} passed, {FAIL} failed ({elapsed:.1f}s)")
    return 0 if FAIL == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
