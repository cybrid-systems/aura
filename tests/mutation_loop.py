#!/usr/bin/env python3
"""
Aura Agent 变异循环 (Mutation Loop)

A closed-loop AI-driven code evolution system. Takes a seed expression,
applies random source-level mutations, verifies correctness, checks for
performance regressions, and iteratively evolves the code.

Usage:
  python3 tests/mutation_loop.py <seed.aura>                # 单次变异
  python3 tests/mutation_loop.py --loop <seed.aura>         # 持续变异
  python3 tests/mutation_loop.py --loop <expr> <expected>   # 持续变异 (内联)
  python3 tests/mutation_loop.py --demo                     # 快速演示
  python3 tests/mutation_loop.py --list                     # 列出变异策略
  python3 tests/mutation_loop.py --loop --fast <seed.aura>  # 持续变异 (无性能检查)

Flags:
  --fast, --no-bench   跳过基准测试回归检测 (debug 构建推荐)
"""

import subprocess
import json
import random
import sys
import os
import re
import time
import shutil
import textwrap
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional


# ── Constants ──────────────────────────────────────────────────

ROOT = Path(__file__).resolve().parent.parent
AURA = ROOT / "build" / "aura"
BENCH = ROOT / "tests" / "benchmark.py"
FIXTURES = ROOT / "tests" / "fixtures"

# Colors
G = "\033[32m"  # green
Y = "\033[33m"  # yellow
R = "\033[31m"  # red
B = "\033[34m"  # blue
C = "\033[36m"  # cyan
N = "\033[0m"   # reset


# ═══════════════════════════════════════════════════════════════
# Data types
# ═══════════════════════════════════════════════════════════════

@dataclass
class Mutation:
    """A single mutation strategy."""
    name: str
    description: str
    apply_fn: callable  # (code: str) → str | None (None if not applicable)


@dataclass
class MutationResult:
    """Result of applying and testing a mutation."""
    mutation_name: str
    success: bool
    code_before: str
    code_after: str
    output_before: str
    output_after: str
    reason: str = ""
    elapsed: float = 0.0


@dataclass
class GenerationReport:
    """Summary of one generation of the mutation loop."""
    generation: int
    total_tried: int
    kept: int
    rejected: int
    no_change: int
    elapsed: float
    current_code: str
    current_output: str
    recent_results: list[MutationResult] = field(default_factory=list)


# ═══════════════════════════════════════════════════════════════
# Aura execution helpers
# ═══════════════════════════════════════════════════════════════

def run_aura(code: str, args: Optional[list[str]] = None,
             timeout: int = 30) -> tuple[int, str, str]:
    """Run aura with given code and arguments.
    Returns (returncode, stdout, stderr).
    """
    cmd = [str(AURA)]
    if args:
        cmd.extend(args)
    try:
        proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True
        )
        stdout, stderr = proc.communicate(code + "\n", timeout=timeout)
        return proc.returncode, stdout.strip(), stderr.strip()
    except subprocess.TimeoutExpired:
        proc.kill()
        return -1, "", "timeout"
    except FileNotFoundError:
        return -1, "", f"binary not found: {AURA}"


def eval_via_serve(code: str) -> tuple[bool, str]:
    """Evaluate code via --serve JSON protocol.
    Returns (success, output_string).
    """
    rc, stdout, stderr = run_aura(code, ["--serve"])
    if rc != 0:
        return False, stderr or f"exit code {rc}"

    # Parse last JSON line of output (the actual response)
    for line in reversed(stdout.strip().split("\n")):
        line = line.strip()
        if not line:
            continue
        try:
            resp = json.loads(line)
            status = resp.get("status", "")
            if status == "ok":
                return True, resp.get("value", "")
            elif status == "error":
                return False, resp.get("msg", "unknown error")
            elif status in ("defined", "redefined"):
                continue  # not a result, skip
            else:
                return False, f"unexpected status: {status}"
        except json.JSONDecodeError:
            # Might be PM log lines mixed in
            continue

    return False, f"no JSON response in: {stdout[:200]}"


def eval_via_ir(code: str) -> tuple[bool, str]:
    """Evaluate code via --ir pipeline.
    Returns (success, output_string).
    """
    rc, stdout, stderr = run_aura(code, ["--ir"])
    if rc != 0 or stderr:
        return False, stderr or stdout
    return True, stdout.strip()


def run_query(code: str, query: str) -> int:
    """Run --query and return match count."""
    rc, stdout, stderr = run_aura(code, ["--query", query])
    if rc != 0:
        return 0
    # Parse "query: N matches"
    m = re.search(r"query:\s*(\d+)\s*matches?", stdout)
    return int(m.group(1)) if m else 0


def run_query_and_fix(code: str, query: str, fix: str) -> tuple[bool, int, int]:
    """Run --query-and-fix. Returns (applied, match_count, patch_count)."""
    rc, stdout, stderr = run_aura(code, ["--query-and-fix", query, fix])
    # Parse "transform: N matches, M patches, applied=X"
    m = re.search(r"transform:\s*(\d+)\s*matches,\s*(\d+)\s*patches,\s*applied=(\w+)", stdout)
    if m:
        return m.group(3) == "true", int(m.group(1)), int(m.group(2))
    return False, 0, 0


def benchmark_check() -> bool:
    """Run benchmark --check, return True if no regression detected."""
    result = subprocess.run(
        [sys.executable, str(BENCH), "--check"],
        capture_output=True, text=True, timeout=120
    )
    return result.returncode == 0


def benchmark_update():
    """Update benchmark baseline."""
    subprocess.run(
        [sys.executable, str(BENCH), "--update"],
        capture_output=True, text=True, timeout=120
    )


# ═══════════════════════════════════════════════════════════════
# Mutation strategies
# ═══════════════════════════════════════════════════════════════

def _strip_comments(code: str) -> str:
    """Strip line comments (; ...) from source code."""
    lines = code.split("\n")
    result = []
    for line in lines:
        # Remove from first ; (but not inside strings — simplified for Aura)
        idx = line.find(";")
        if idx >= 0:
            line = line[:idx]
        result.append(line)
    cleaned = "\n".join(result).strip()
    # Remove any orphaned parentheses from partial comment removal
    # but generally should be fine
    return cleaned


def mut_increment_ints(code: str) -> Optional[str]:
    """Increment all integer literals by 1."""
    clean = _strip_comments(code)
    def inc(m):
        try:
            val = int(m.group(0))
            return str(val + 1)
        except ValueError:
            return m.group(0)
    result = re.sub(r'\b(\d+)\b', inc, clean)
    return result if result != clean else None


def mut_decrement_ints(code: str) -> Optional[str]:
    """Decrement all integer literals by 1."""
    clean = _strip_comments(code)
    def dec(m):
        try:
            val = int(m.group(0))
            return str(val - 1)
        except ValueError:
            return m.group(0)
    result = re.sub(r'\b(\d+)\b', dec, clean)
    return result if result != clean else None


def mut_double_ints(code: str) -> Optional[str]:
    """Double all integer literals."""
    clean = _strip_comments(code)
    def dbl(m):
        try:
            val = int(m.group(0))
            return str(val * 2)
        except ValueError:
            return m.group(0)
    result = re.sub(r'\b(\d+)\b', dbl, clean)
    return result if result != clean else None


def mut_swap_add_mul(code: str) -> Optional[str]:
    """Replace + with * (operator swap)."""
    clean = _strip_comments(code)
    result = clean.replace("+", "\x00SWAP\x00")
    result = result.replace("*", "+")
    result = result.replace("\x00SWAP\x00", "*")
    return result if result != clean else None


def mut_swap_add_sub(code: str) -> Optional[str]:
    """Replace + with - (operator swap)."""
    clean = _strip_comments(code)
    result = clean.replace("+", "\x00SWAP\x00")
    result = result.replace("-", "+")
    result = result.replace("\x00SWAP\x00", "-")
    return result if result != clean else None


def mut_add_redundant_zero(code: str) -> Optional[str]:
    """Wrap the last expression in (+ <expr> 0)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    result = f"(+ {clean} 0)"
    return result if result != code else None


def mut_add_redundant_one(code: str) -> Optional[str]:
    """Wrap the last expression in (* <expr> 1)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    result = f"(* {clean} 1)"
    return result if result != code else None


def mut_swap_branches(code: str) -> Optional[str]:
    """Swap then/else branches of the innermost if expression."""
    clean = _strip_comments(code)
    # Match (if COND THEN ELSE) and swap THEN and ELSE
    m = re.search(r'\(if\s+(.*?)\)\s*$', clean)
    if m:
        inner = m.group(1)
        parts = _split_if_arms(inner)
        if parts and len(parts) == 3:
            cond, then_br, else_br = parts
            result = clean[:m.start()] + f"(if {cond} {else_br} {then_br})" + clean[m.end():]
            return result if result != clean else None
    return None


def _split_if_arms(s: str) -> Optional[list[str]]:
    """Split (cond then else) arms respecting paren balance."""
    s = s.strip()
    parts = []
    depth = 0
    current = ""
    for c in s:
        if c == '(':
            depth += 1
            current += c
        elif c == ')':
            depth -= 1
            current += c
        elif c == ' ' and depth == 0:
            if current:
                parts.append(current.strip())
                current = ""
            continue
        else:
            current += c
    if current:
        parts.append(current.strip())
    return parts if len(parts) == 3 else None


def mut_add_let_wrapper(code: str) -> Optional[str]:
    """Wrap expression in (let ((x <expr>)) x)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    var = random.choice(["x", "y", "z", "tmp", "val", "result"])
    result = f"(let (({var} {clean})) {var})"
    return result if result != code else None


# ── AST-level mutations (via --query-and-fix) ────────────────

def mut_ast_int_replacement(code: str) -> Optional[str]:
    """AST mutation: replace all LiteralInt with LiteralInt(0) via --query-and-fix."""
    applied, matches, patches = run_query_and_fix(code,
        "(node-type LiteralInt)", "(LiteralInt 0)")
    if applied and matches > 0:
        # Since we can't get the modified source back, we do text-level:
        return re.sub(r'\b\d+\b', '0', code)
    return None


def mut_add_type_annotation(code: str) -> Optional[str]:
    """Add a type annotation comment (no-op transformation).
    Note: comments are stripped before text operations. This mutation
    adds a comment for annotation, but text-level transforms operate
    on cleaned code only.
    """
    # This is an identity mutation — for display purposes only.
    # Actual text operations strip comments, so this doesn't affect
    # subsequent mutations.
    return None  # Disabled: multi-line comments break text mutations


# ── Registry ──────────────────────────────────────────────────

MUTATIONS: list[Mutation] = [
    Mutation("inc_ints",       "Increment all integer literals by 1",                    mut_increment_ints),
    Mutation("dec_ints",       "Decrement all integer literals by 1",                    mut_decrement_ints),
    Mutation("double_ints",    "Double all integer literals",                            mut_double_ints),
    Mutation("swap_add_mul",   "Swap + and * operators",                                mut_swap_add_mul),
    Mutation("swap_add_sub",   "Replace + with -",                                      mut_swap_add_sub),
    Mutation("add_zero",       "Wrap in (+ <expr> 0) (semantics-preserving)",            mut_add_redundant_zero),
    Mutation("add_one",        "Wrap in (* <expr> 1) (semantics-preserving)",            mut_add_redundant_one),
    Mutation("swap_if_branches","Swap then/else branches of innermost if",               mut_swap_branches),
    Mutation("wrap_let",       "Introduce trivial let binding",                          mut_add_let_wrapper),
    Mutation("add_comment",    "[DISABLED] Would add type annotation comment",             mut_add_type_annotation),
    # AST-level (via --query-and-fix verification)
    Mutation("int_to_zero",    "Set all int literals to 0 (AST verified)",               mut_ast_int_replacement),
]


# ═══════════════════════════════════════════════════════════════
# Mutation loop core
# ═══════════════════════════════════════════════════════════════

def get_expected_output(code: str) -> tuple[bool, str]:
    """Determine the expected output for a code expression.
    Returns (success, output_string).
    """
    # Try --serve first, fall back to --ir
    ok, out = eval_via_serve(code)
    if ok:
        return True, out
    ok, out = eval_via_ir(code)
    if ok:
        return True, out
    return False, out


def get_output(code: str) -> tuple[bool, str]:
    """Get output using --ir (faster than --serve)."""
    ok, out = eval_via_ir(code)
    if ok:
        return True, out
    # Fall back to --serve
    return eval_via_serve(code)


def apply_mutation(code: str, mutation: Mutation) -> tuple[bool, str]:
    """Apply a single mutation strategy. Returns (success, new_code)."""
    try:
        result = mutation.apply_fn(code)
        if result is None:
            return False, code
        return True, result
    except Exception as e:
        return False, code


def test_mutation(
    code: str,
    expected_output: str,
    mutation: Mutation,
    mutation_idx: int,
    total_mutations: int,
    do_bench: bool = True
) -> MutationResult:
    """Apply a mutation, test it, and return the result."""
    start = time.time()

    # Apply
    ok, new_code = apply_mutation(code, mutation)
    if not ok or new_code == code:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name, False, code, code,
            expected_output, expected_output,
            reason="no change", elapsed=elapsed
        )

    # Test execution
    ok2, output = get_output(new_code)
    if not ok2:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name, False, code, new_code,
            expected_output, output or "exec error",
            reason=f"execution failed", elapsed=elapsed
        )

    # Verify against expected
    if output != expected_output:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name, False, code, new_code,
            expected_output, output,
            reason=f"output mismatch: got '{output}', expected '{expected_output}'",
            elapsed=elapsed
        )

    # Benchmark regression check
    # Note: disabled by default in --fast mode. Debug builds have high
    # variance (~1.5× for trivial changes), making per-mutation regression
    # checks impractical. Use a final `benchmark.py --check` instead.
    if do_bench:
        ok3 = benchmark_check()
        if not ok3:
            elapsed = time.time() - start
            return MutationResult(
                mutation.name, False, code, new_code,
                expected_output, output,
                reason="benchmark regression", elapsed=elapsed
            )

    elapsed = time.time() - start
    return MutationResult(
        mutation.name, True, code, new_code,
        expected_output, output,
        reason="accepted", elapsed=elapsed
    )


def single_pass(
    code: str,
    expected: str,
    do_bench: bool = True,
    seed: Optional[int] = None
) -> GenerationReport:
    """Run one pass: try all mutations, keep the first good one."""
    if seed is not None:
        random.seed(seed)

    tried = 0
    kept = 0
    rejected = 0
    no_change = 0
    results = []
    start = time.time()

    # Shuffle mutations for variety
    mutations = list(MUTATIONS)
    random.shuffle(mutations)

    for idx, mutation in enumerate(mutations):
        tried += 1
        result = test_mutation(code, expected, mutation, idx, len(mutations), do_bench)

        if result.success:
            kept += 1
            results.append(result)
            current_code = result.code_after
            current_output = expected
            print(f"  {G}✓{N} [{result.mutation_name:15s}] {result.reason} ({result.elapsed:.2f}s)")
            break
        elif result.reason == "no change":
            no_change += 1
            print(f"  {Y}~{N} [{result.mutation_name:15s}] {result.reason}")
        else:
            rejected += 1
            print(f"  {R}✗{N} [{result.mutation_name:15s}] {result.reason[:60]} ({result.elapsed:.2f}s)")
    else:
        # No mutation was kept
        current_code = code
        current_output = expected

    elapsed = time.time() - start
    return GenerationReport(
        generation=0,
        total_tried=tried,
        kept=kept,
        rejected=rejected,
        no_change=no_change,
        elapsed=elapsed,
        current_code=current_code,
        current_output=current_output,
        recent_results=results
    )


def mutation_loop(
    seed_code: str,
    seed_expected: Optional[str] = None,
    iterations: int = 10,
    do_bench: bool = False,
    verbose: bool = True,
    random_seed: int = 42
) -> str:
    """Run the mutation loop for N iterations.

    Each iteration:
      1. Tries random mutation strategies (shuffled)
      2. Applies the first one that produces a valid change
      3. Verifies output matches expected
      4. If verified, keeps it (updates baseline if benchmark enabled)
      5. If failed, discards and tries another

    Returns the final evolved code.

    Note: `do_bench=False` by default because per-mutation benchmark
    checks are too noisy on debug builds. Run `benchmark.py --check`
    separately after the loop for a clean assessment.
    """
    random.seed(random_seed)

    # Determine expected output from seed
    if seed_expected is None:
        ok, seed_expected = get_output(seed_code)
        if not ok:
            print(f"{R}Error: cannot determine expected output for seed{N}")
            print(f"  Seed: {seed_code}")
            print(f"  Error: {seed_expected}")
            sys.exit(1)

    print(f"{C}{'═'*60}{N}")
    print(f"{B}Aura 变异循环 (Mutation Loop){N}")
    print(f"{C}{'═'*60}{N}")
    print(f"  Seed:       {Y}{seed_code}{N}")
    print(f"  Expected:   {G}{seed_expected}{N}")
    print(f"  Iterations: {iterations}")
    print(f"  Benchmark:  {'on' if do_bench else 'off'}")
    print(f"  Random seed: {random_seed}")
    print(f"{C}{'─'*60}{N}")

    current_code = seed_code
    current_expected = seed_expected

    for gen in range(iterations):
        print(f"\n{B}Generation {gen + 1}/{iterations}{N}")
        print(f"  Current: {Y}{current_code[:80]}{N}")

        report = single_pass(current_code, current_expected, do_bench, random_seed + gen)

        if report.kept > 0:
            current_code = report.current_code
            print(f"  {G}→ Kept:{N} {current_code[:80]}")
            # Update benchmark baseline for kept mutation
            if do_bench:
                benchmark_update()
        else:
            print(f"  {Y}→ No mutation kept this generation{N}")

        # Print summary
        k = report.kept
        r = report.rejected
        n = report.no_change
        total = report.total_tried
        pct = (k / total * 100) if total > 0 else 0
        print(f"  {B}Summary:{N} {k} kept, {r} rejected, {n} no-change ({pct:.0f}% success, {report.elapsed:.2f}s)")

    print(f"\n{C}{'═'*60}{N}")
    print(f"{G}Mutation loop complete!{N}")
    print(f"  Final code: {Y}{current_code}{N}")
    print(f"  Output:     {G}{current_expected}{N}")
    print(f"{C}{'═'*60}{N}")

    return current_code


# ═══════════════════════════════════════════════════════════════
# Demo mode
# ═══════════════════════════════════════════════════════════════

def run_demo():
    """Run a quick demo showing each mutation strategy."""
    print(f"{C}{'═'*60}{N}")
    print(f"{B}Aura 变异循环 — Quick Demo{N}")
    print(f"{C}{'═'*60}{N}")

    seed = "(+ 1 2)"
    ok, expected = get_output(seed)
    print(f"  Seed:     {Y}{seed}{N}")
    print(f"  Expected: {G}{expected}{N}")
    print(f"{C}{'─'*60}{N}")

    print(f"\n{B}Mutation Strategies:{N}\n")

    for mut in MUTATIONS:
        result = mut.apply_fn(seed)
        if result and result != seed:
            ok2, out = get_output(result)
            status = f"{G}✓{N}" if ok2 and out == expected else f"{R}✗{N}"
            print(f"  {status} {mut.name:15s} → {Y}{result[:50]}{N}")
            if ok2 and out != expected:
                print(f"    {'':17s} output: {out} (expected: {expected})")
        else:
            print(f"  {Y}~{N} {mut.name:15s} → not applicable")

    print(f"{C}{'─'*60}{N}")
    print(f"\n{B}Demo complete.{N}")


def list_mutations():
    """Print the list of available mutation strategies."""
    print(f"{B}Available Mutation Strategies:{N}\n")
    for mut in MUTATIONS:
        print(f"  {mut.name:18s} {mut.description}")
    print(f"\n{B}Total:{N} {len(MUTATIONS)} strategies")


# ═══════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════

def main():
    if not AURA.exists():
        print(f"{R}Error: {AURA} not found. Run 'python3 build.py build' first.{N}")
        sys.exit(1)

    args = sys.argv[1:]

    # Parse --fast / --no-bench flags
    do_bench = True
    if "--fast" in args or "--no-bench" in args:
        do_bench = False
        args = [a for a in args if a not in ("--fast", "--no-bench")]

    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip())
        return

    cmd = args[0]

    if cmd == "--demo":
        run_demo()
        return

    if cmd == "--list":
        list_mutations()
        return

    if cmd == "--loop":
        # --loop <seed.aura> or --loop <expr> <expected>
        if len(args) >= 3:
            seed = args[1]
            expected = args[2]
        elif len(args) == 2:
            path = Path(args[1])
            if path.exists():
                seed = path.read_text().strip()
                ok, expected = get_output(seed)
                if not ok:
                    print(f"{R}Error: cannot evaluate seed '{seed}': {expected}{N}")
                    sys.exit(1)
            else:
                seed = args[1]
                ok, expected = get_output(seed)
                if not ok:
                    print(f"{R}Error: cannot evaluate '{seed}': {expected}{N}")
                    print(f"  If it's a file path, it doesn't exist: {path}")
                    sys.exit(1)
        else:
            print(f"Usage: {sys.argv[0]} --loop <seed.aura>")
            print(f"       {sys.argv[0]} --loop <expr> <expected>")
            sys.exit(1)

        mutation_loop(seed, expected, iterations=10, do_bench=do_bench)
        return

    # Default: single mutation pass
    path = Path(args[0])
    if path.exists():
        code = path.read_text().strip()
    else:
        code = args[0]

    print(f"{B}Single mutation pass{N}")
    print(f"  Input: {Y}{code}{N}")

    ok, expected = get_output(code)
    if not ok:
        print(f"{R}Error: cannot evaluate input: {expected}{N}")
        sys.exit(1)
    print(f"  Output: {G}{expected}{N}\n")

    random.seed(0)
    mutations = list(MUTATIONS)
    random.shuffle(mutations)

    for idx, mut in enumerate(mutations):
        result = test_mutation(code, expected, mut, idx, len(mutations), do_bench=do_bench)
        icon = G + "✓" if result.success else (R + "✗" if result.reason != "no change" else Y + "~")
        print(f"  {icon}{N} [{result.mutation_name:15s}] {result.reason[:70]}")

    return


if __name__ == "__main__":
    main()
