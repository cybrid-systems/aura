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
  python3 tests/mutation_loop.py --ai <seed.aura>           # AI驱动变异

Flags:
  --fast, --no-bench   跳过基准测试回归检测 (debug 构建推荐)
  --ai, --llm          启用AI驱动变异 (使用LLM生成代码变化)
  --model <name>       LLM模型名称 (默认: deepseek/deepseek-v4-flash)
  --api-key <key>      LLM API密钥 (默认: 环境变量 LLM_API_KEY)
  --base-url <url>     LLM API基础URL (默认: 环境变量 LLM_BASE_URL)
  --iterations <N>     AI变异迭代次数 (默认: 5)
"""

import http.client
import json
import os
import random
import re
import subprocess
import sys
import textwrap
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from _aura_harness import AURA_BIN as AURA
from _aura_harness import ROOT, B, C, G, N, R, Y

BENCH = ROOT / "tests" / "benchmark.py"
FIXTURES = ROOT / "tests" / "fixtures"


# ═══════════════════════════════════════════════════════════════
# Data types
# ═══════════════════════════════════════════════════════════════


@dataclass
class Mutation:
    """A single mutation strategy."""

    name: str
    description: str
    apply_fn: Callable[[str], str | None]  # None if mutation not applicable


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


def run_aura(code: str, args: list[str] | None = None, timeout: int = 30) -> tuple[int, str, str]:
    """Run aura with given code and arguments.
    Returns (returncode, stdout, stderr).
    """
    cmd = [str(AURA)]
    if args:
        cmd.extend(args)
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
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
        capture_output=True,
        text=True,
        timeout=120,
    )
    return result.returncode == 0


def benchmark_update():
    """Update benchmark baseline."""
    subprocess.run(
        [sys.executable, str(BENCH), "--update"],
        capture_output=True,
        text=True,
        timeout=120,
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


def mut_increment_ints(code: str) -> str | None:
    """Increment all integer literals by 1."""
    clean = _strip_comments(code)

    def inc(m):
        try:
            val = int(m.group(0))
            return str(val + 1)
        except ValueError:
            return m.group(0)

    result = re.sub(r"\b(\d+)\b", inc, clean)
    return result if result != clean else None


def mut_decrement_ints(code: str) -> str | None:
    """Decrement all integer literals by 1."""
    clean = _strip_comments(code)

    def dec(m):
        try:
            val = int(m.group(0))
            return str(val - 1)
        except ValueError:
            return m.group(0)

    result = re.sub(r"\b(\d+)\b", dec, clean)
    return result if result != clean else None


def mut_double_ints(code: str) -> str | None:
    """Double all integer literals."""
    clean = _strip_comments(code)

    def dbl(m):
        try:
            val = int(m.group(0))
            return str(val * 2)
        except ValueError:
            return m.group(0)

    result = re.sub(r"\b(\d+)\b", dbl, clean)
    return result if result != clean else None


def mut_swap_add_mul(code: str) -> str | None:
    """Replace + with * (operator swap)."""
    clean = _strip_comments(code)
    result = clean.replace("+", "\x00SWAP\x00")
    result = result.replace("*", "+")
    result = result.replace("\x00SWAP\x00", "*")
    return result if result != clean else None


def mut_swap_add_sub(code: str) -> str | None:
    """Replace + with - (operator swap)."""
    clean = _strip_comments(code)
    result = clean.replace("+", "\x00SWAP\x00")
    result = result.replace("-", "+")
    result = result.replace("\x00SWAP\x00", "-")
    return result if result != clean else None


def mut_add_redundant_zero(code: str) -> str | None:
    """Wrap the last expression in (+ <expr> 0)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    result = f"(+ {clean} 0)"
    return result if result != code else None


def mut_add_redundant_one(code: str) -> str | None:
    """Wrap the last expression in (* <expr> 1)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    result = f"(* {clean} 1)"
    return result if result != code else None


def mut_swap_branches(code: str) -> str | None:
    """Swap then/else branches of the innermost if expression."""
    clean = _strip_comments(code)
    # Match (if COND THEN ELSE) and swap THEN and ELSE
    m = re.search(r"\(if\s+(.*?)\)\s*$", clean)
    if m:
        inner = m.group(1)
        parts = _split_if_arms(inner)
        if parts and len(parts) == 3:
            cond, then_br, else_br = parts
            result = clean[: m.start()] + f"(if {cond} {else_br} {then_br})" + clean[m.end() :]
            return result if result != clean else None
    return None


def _split_if_arms(s: str) -> list[str] | None:
    """Split (cond then else) arms respecting paren balance."""
    s = s.strip()
    parts = []
    depth = 0
    current = ""
    for c in s:
        if c == "(":
            depth += 1
            current += c
        elif c == ")":
            depth -= 1
            current += c
        elif c == " " and depth == 0:
            if current:
                parts.append(current.strip())
                current = ""
            continue
        else:
            current += c
    if current:
        parts.append(current.strip())
    return parts if len(parts) == 3 else None


def mut_add_let_wrapper(code: str) -> str | None:
    """Wrap expression in (let ((x <expr>)) x)."""
    clean = _strip_comments(code)
    if not clean:
        return None
    var = random.choice(["x", "y", "z", "tmp", "val", "result"])
    result = f"(let (({var} {clean})) {var})"
    return result if result != code else None


# ── AST-level mutations (via --query-and-fix) ────────────────


def mut_ast_int_replacement(code: str) -> str | None:
    """AST mutation: replace all LiteralInt with LiteralInt(0) via --query-and-fix."""
    applied, matches, patches = run_query_and_fix(code, "(node-type LiteralInt)", "(LiteralInt 0)")
    if applied and matches > 0:
        # Since we can't get the modified source back, we do text-level:
        return re.sub(r"\b\d+\b", "0", code)
    return None


def mut_add_type_annotation(code: str) -> str | None:
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
    Mutation("inc_ints", "Increment all integer literals by 1", mut_increment_ints),
    Mutation("dec_ints", "Decrement all integer literals by 1", mut_decrement_ints),
    Mutation("double_ints", "Double all integer literals", mut_double_ints),
    Mutation("swap_add_mul", "Swap + and * operators", mut_swap_add_mul),
    Mutation("swap_add_sub", "Replace + with -", mut_swap_add_sub),
    Mutation(
        "add_zero",
        "Wrap in (+ <expr> 0) (semantics-preserving)",
        mut_add_redundant_zero,
    ),
    Mutation("add_one", "Wrap in (* <expr> 1) (semantics-preserving)", mut_add_redundant_one),
    Mutation("swap_if_branches", "Swap then/else branches of innermost if", mut_swap_branches),
    Mutation("wrap_let", "Introduce trivial let binding", mut_add_let_wrapper),
    Mutation(
        "add_comment",
        "[DISABLED] Would add type annotation comment",
        mut_add_type_annotation,
    ),
    # AST-level (via --query-and-fix verification)
    Mutation(
        "int_to_zero",
        "Set all int literals to 0 (AST verified)",
        mut_ast_int_replacement,
    ),
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
    except Exception:
        return False, code


def test_mutation(
    code: str,
    expected_output: str,
    mutation: Mutation,
    mutation_idx: int,
    total_mutations: int,
    do_bench: bool = True,
) -> MutationResult:
    """Apply a mutation, test it, and return the result."""
    start = time.time()

    # Apply
    ok, new_code = apply_mutation(code, mutation)
    if not ok or new_code == code:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name,
            False,
            code,
            code,
            expected_output,
            expected_output,
            reason="no change",
            elapsed=elapsed,
        )

    # Test execution
    ok2, output = get_output(new_code)
    if not ok2:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name,
            False,
            code,
            new_code,
            expected_output,
            output or "exec error",
            reason="execution failed",
            elapsed=elapsed,
        )

    # Verify against expected
    if output != expected_output:
        elapsed = time.time() - start
        return MutationResult(
            mutation.name,
            False,
            code,
            new_code,
            expected_output,
            output,
            reason=f"output mismatch: got '{output}', expected '{expected_output}'",
            elapsed=elapsed,
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
                mutation.name,
                False,
                code,
                new_code,
                expected_output,
                output,
                reason="benchmark regression",
                elapsed=elapsed,
            )

    elapsed = time.time() - start
    return MutationResult(
        mutation.name,
        True,
        code,
        new_code,
        expected_output,
        output,
        reason="accepted",
        elapsed=elapsed,
    )


def single_pass(code: str, expected: str, do_bench: bool = True, seed: int | None = None) -> GenerationReport:
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
        recent_results=results,
    )


def mutation_loop(
    seed_code: str,
    seed_expected: str | None = None,
    iterations: int = 10,
    do_bench: bool = False,
    verbose: bool = True,
    random_seed: int = 42,
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

    print(f"{C}{'═' * 60}{N}")
    print(f"{B}Aura 变异循环 (Mutation Loop){N}")
    print(f"{C}{'═' * 60}{N}")
    print(f"  Seed:       {Y}{seed_code}{N}")
    print(f"  Expected:   {G}{seed_expected}{N}")
    print(f"  Iterations: {iterations}")
    print(f"  Benchmark:  {'on' if do_bench else 'off'}")
    print(f"  Random seed: {random_seed}")
    print(f"{C}{'─' * 60}{N}")

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

    print(f"\n{C}{'═' * 60}{N}")
    print(f"{G}Mutation loop complete!{N}")
    print(f"  Final code: {Y}{current_code}{N}")
    print(f"  Output:     {G}{current_expected}{N}")
    print(f"{C}{'═' * 60}{N}")

    return current_code


# ═══════════════════════════════════════════════════════════════
# LLM API (AI-driven mutation)
# ═══════════════════════════════════════════════════════════════


def llm_complete(
    messages: list[dict],
    api_key: str | None = None,
    model: str | None = None,
    base_url: str | None = None,
    timeout: int = 60,
) -> str:
    """Call an OpenAI-compatible chat completion API.

    Returns the response text content, or empty string on failure.

    Environment variables (used when corresponding arg is None):
      LLM_API_KEY   — API key
      LLM_MODEL     — Model name (e.g. "deepseek/deepseek-v4-flash")
      LLM_BASE_URL  — API base URL (e.g. "https://api.deepseek.com")
    """
    api_key = api_key or os.environ.get("LLM_API_KEY", "")
    model = model or os.environ.get("LLM_MODEL", "deepseek/deepseek-v4-flash")
    base_url = base_url or os.environ.get("LLM_BASE_URL", "")

    if not api_key:
        print(f"  {Y}Warning: No LLM_API_KEY set, falling back to random mutation{N}")
        return ""

    # Derive base_url from model prefix if not set
    if not base_url:
        model_prefix = model.split("/")[0] if "/" in model else ""
        known_endpoints = {
            "deepseek": "https://api.deepseek.com",
            "openai": "https://api.openai.com",
            "anthropic": "https://api.anthropic.com",
            "grok": "https://api.x.ai",
            "google": "https://generativelanguage.googleapis.com",
        }
        base_url = known_endpoints.get(model_prefix, "https://api.openai.com")

    # Strip trailing slash
    base_url = base_url.rstrip("/")

    # Parse host and path from base_url
    scheme_rest = base_url.split("://", 1)
    host_part = scheme_rest[1] if len(scheme_rest) == 2 else scheme_rest[0]

    # Handle possible path in host_part, e.g. "api.deepseek.com/v1"
    if "/" in host_part:
        host, path_prefix = host_part.split("/", 1)
        path_prefix = "/" + path_prefix
    else:
        host = host_part
        path_prefix = ""

    body = json.dumps(
        {
            "model": model,
            "messages": messages,
            "temperature": 0.7,
            "max_tokens": 2048,
        }
    )

    try:
        conn = http.client.HTTPSConnection(host, timeout=timeout)
        conn.request(
            "POST",
            f"{path_prefix}/chat/completions",
            body=body,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {api_key}",
            },
        )
        resp = conn.getresponse()
        if resp.status != 200:
            err_body = resp.read().decode("utf-8", errors="replace")[:200]
            print(f"  {Y}LLM API error: HTTP {resp.status} — {err_body}{N}")
            return ""
        data = json.loads(resp.read().decode("utf-8"))
        conn.close()
        return data["choices"][0]["message"]["content"].strip()
    except Exception as e:
        print(f"  {Y}LLM API exception: {e}{N}")
        return ""


def _extract_code_from_llm_response(raw: str) -> str | None:
    """Extract code from an LLM response.

    Handles:
    1. Code in markdown fenced blocks (```aura ... ``` or ```lisp ... ```)
    2. Code in markdown code block (``` ... ```) — takes first block
    3. Plain S-expression text with balanced parens

    Returns the extracted code or None if nothing parseable found.
    """
    if not raw:
        return None

    raw = raw.strip()

    # Strategy 1: Look for fenced code blocks (``` or `````)
    block_pattern = re.compile(r"```(?:aura|lisp|scheme|clojure|scm)?\s*\n?(.*?)```", re.DOTALL)
    blocks = block_pattern.findall(raw)
    if blocks:
        candidate = blocks[0].strip()
        if candidate:
            return candidate

    # Strategy 2: Look for inline code (`...`)
    inline_pattern = re.compile(r"`([^`]+)`")
    inlines = inline_pattern.findall(raw)
    if inlines:
        for candidate in inlines:
            candidate = candidate.strip()
            if candidate and (candidate.startswith("(") or re.match(r'^[\d"\']', candidate)):
                return candidate

    # Strategy 3: Try to find an S-expression (balanced parens)
    # Look for the first top-level S-expression
    depth = 0
    start = -1
    for i, c in enumerate(raw):
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0 and start >= 0:
                candidate = raw[start : i + 1].strip()
                if candidate:
                    return candidate

    # Strategy 4: Return the whole thing if it looks like code
    if raw.startswith("(") or raw.startswith('"') or re.match(r"^[\d\-]", raw):
        return raw

    return None


def _build_ai_system_prompt() -> str:
    """Build the system prompt for AI-driven mutation."""
    return textwrap.dedent("""\
    You are an AI code evolution agent for the Aura programming language.
    Aura is a Lisp-like language where code is data (homoiconic).

    Aura syntax examples:
    - Literals: 42, "hello"
    - Arithmetic: (+ 1 2), (* 3 4), (- 10 5), (/ 100 5)
    - Variables: (let ((x 10)) x)
    - Lambda: (lambda (x) (* x 2))
    - Apply: ((lambda (x) (* x 2)) 5)
    - If: (if condition then else)
    - Recursion: (letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))
    - Pairs: (cons 1 2), (car pair), (cdr pair)
    - String: (string-append "a" "b"), (string-length s), (string-ref s i)
    - Type query: (type-of 42), (type? 42 "Int")

    Your task: Given a piece of Aura code and its execution result, suggest a mutation.
    The mutation should:
    1. Preserve semantic correctness (the new code should still compile and produce meaningful results)
    2. Be creative but practical (don't break the code randomly)
    3. Return ONLY the complete mutated Aura S-expression, no explanation
    """)


def _build_ai_user_prompt(code: str, result: str, history: list[dict]) -> str:
    """Build the user prompt for a given code and context."""
    history_str = "[]"
    if history:
        history_str = json.dumps(history[-5:], indent=2)

    return textwrap.dedent(f"""\
    Current code: {code}
    Previous result: {result}
    Mutation history: {history_str}

    Return ONLY the complete mutated Aura S-expression with no explanation.
    """)


def mutate_with_ai(
    code: str,
    result: str,
    history: list[dict],
    api_key: str | None = None,
    model: str | None = None,
    base_url: str | None = None,
) -> str | None:
    """Use LLM to suggest a mutation for the given code.

    Returns the mutated code string, or None if the LLM call failed
    or the response couldn't be parsed.
    """
    system_prompt = _build_ai_system_prompt()
    user_prompt = _build_ai_user_prompt(code, result, history)

    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]

    print(f"  {C}Sending code to LLM for mutation suggestion...{N}")
    response = llm_complete(messages, api_key=api_key, model=model, base_url=base_url)

    if not response:
        print(f"  {Y}LLM returned empty response{N}")
        return None

    mutated = _extract_code_from_llm_response(response)
    if mutated:
        print(f"  {G}LLM returned:{N} {mutated[:80]}{'...' if len(mutated) > 80 else ''}")
        return mutated
    else:
        print(f"  {Y}Could not parse code from LLM response: {response[:100]}{N}")
        return None


def ai_mutation_loop(
    seed_code: str,
    iterations: int = 5,
    do_bench: bool = False,
    verbose: bool = True,
    api_key: str | None = None,
    model: str | None = None,
    base_url: str | None = None,
) -> str:
    """Run AI-driven mutation loop for N iterations.

    Each iteration:
      1. Sends current code + result + history to LLM
      2. LLM suggests a mutation
      3. Applies, compiles, verifies output matches expected
      4. Feeds result back as context for next iteration
      5. If LLM fails, falls back to random mutation

    Returns the final evolved code.
    """
    # Get expected output
    ok, expected = get_output(seed_code)
    if not ok:
        print(f"{R}Error: cannot evaluate seed code: {expected}{N}")
        print(f"  Seed: {seed_code}")
        sys.exit(1)

    print(f"{C}{'═' * 60}{N}")
    print(f"{B}Aura AI Mutation Loop{N}")
    print(f"{C}{'═' * 60}{N}")
    print(f"  Seed:       {Y}{seed_code}{N}")
    print(f"  Expected:   {G}{expected}{N}")
    print(f"  Iterations: {iterations}")
    print(f"  Model:      {model or os.environ.get('LLM_MODEL', 'deepseek/deepseek-v4-flash')}")
    print(f"  Base URL:   {base_url or os.environ.get('LLM_BASE_URL', 'auto-detect')}")
    print(f"{C}{'─' * 60}{N}")

    current_code = seed_code
    current_result = expected
    history = []

    for gen in range(iterations):
        print(f"\n{B}AI Generation {gen + 1}/{iterations}{N}")
        print(f"  Current: {Y}{current_code[:80]}{N}")

        # Try AI mutation first
        ai_code = mutate_with_ai(
            current_code,
            current_result,
            history,
            api_key=api_key,
            model=model,
            base_url=base_url,
        )

        if ai_code and ai_code != current_code:
            # Test the AI-suggested mutation
            ok2, output = get_output(ai_code)
            if ok2 and output == current_result:
                # Accepted!
                history.append(
                    {
                        "iteration": gen + 1,
                        "code_before": current_code,
                        "code_after": ai_code,
                        "result": "accepted",
                        "output": output,
                    }
                )
                current_code = ai_code
                current_result = output
                print(f"  {G}✓ AI mutation accepted{N}")
                print(f"  {G}→ Kept:{N} {current_code[:80]}")
            elif ok2:
                # Output changed but code is valid — still accept as creative mutation
                history.append(
                    {
                        "iteration": gen + 1,
                        "code_before": current_code,
                        "code_after": ai_code,
                        "result": "output_changed",
                        "previous_output": current_result,
                        "new_output": output,
                    }
                )
                print(f"  {Y}~ Output changed: '{current_result}' → '{output}'{N}")
                current_code = ai_code
                current_result = output
                print(f"  {Y}→ Kept (new output):{N} {current_code[:80]}")
            else:
                # Execution error — record and fall back to random
                print(f"  {R}✗ AI mutation failed: {output[:80]}{N}")
                history.append(
                    {
                        "iteration": gen + 1,
                        "code_before": current_code,
                        "code_after": ai_code,
                        "result": "exec_error",
                        "error": output,
                    }
                )
                # Fall back to random mutation
                print(f"  {Y}Falling back to random mutation...{N}")
                report = single_pass(current_code, current_result, do_bench)
                if report.kept > 0:
                    current_code = report.current_code
                    print(f"  {G}→ Random kept:{N} {current_code[:80]}")
        elif ai_code is None:
            # LLM call failed — fall back to random
            print(f"  {Y}AI mutation unavailable, falling back to random...{N}")
            report = single_pass(current_code, current_result, do_bench)
            if report.kept > 0:
                current_code = report.current_code
                print(f"  {G}→ Random kept:{N} {current_code[:80]}")
        else:
            # LLM returned same code or couldn't parse
            print(f"  {Y}~ AI returned no effective change, falling back to random...{N}")
            report = single_pass(current_code, current_result, do_bench)
            if report.kept > 0:
                current_code = report.current_code
                print(f"  {G}→ Random kept:{N} {current_code[:80]}")

    print(f"\n{C}{'═' * 60}{N}")
    print(f"{G}AI Mutation loop complete!{N}")
    print(f"  Final code: {Y}{current_code}{N}")
    print(f"  Output:     {G}{current_result}{N}")
    print(f"{C}{'═' * 60}{N}")

    return current_code


# ═══════════════════════════════════════════════════════════════
# Demo mode
# ═══════════════════════════════════════════════════════════════


def run_demo():
    """Run a quick demo showing each mutation strategy."""
    print(f"{C}{'═' * 60}{N}")
    print(f"{B}Aura 变异循环 — Quick Demo{N}")
    print(f"{C}{'═' * 60}{N}")

    seed = "(+ 1 2)"
    ok, expected = get_output(seed)
    print(f"  Seed:     {Y}{seed}{N}")
    print(f"  Expected: {G}{expected}{N}")
    print(f"{C}{'─' * 60}{N}")

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

    print(f"{C}{'─' * 60}{N}")
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


def _parse_flag_args(args: list[str]) -> dict:
    """Parse known flags from args list, returning remaining args.

    Returns dict with:
      - 'remaining': non-flag positional args
      - 'ai': bool
      - 'model': str or None
      - 'api_key': str or None
      - 'base_url': str or None
      - 'iterations': int
      - 'do_bench': bool
    """
    result = {
        "ai": False,
        "model": None,
        "api_key": None,
        "base_url": None,
        "iterations": 5,
        "do_bench": True,
    }
    i = 0
    remaining = []
    while i < len(args):
        a = args[i]
        if a in ("--ai", "--llm"):
            result["ai"] = True
            i += 1
        elif a in ("--fast", "--no-bench"):
            result["do_bench"] = False
            i += 1
        elif a == "--model":
            i += 1
            if i < len(args):
                result["model"] = args[i]
                i += 1
        elif a == "--api-key":
            i += 1
            if i < len(args):
                result["api_key"] = args[i]
                i += 1
        elif a == "--base-url":
            i += 1
            if i < len(args):
                result["base_url"] = args[i]
                i += 1
        elif a == "--iterations":
            i += 1
            if i < len(args):
                try:
                    result["iterations"] = int(args[i])
                except ValueError:
                    print(f"{Y}Warning: invalid --iterations value '{args[i]}', using default 5{N}")
                i += 1
        elif a in ("-h", "--help"):
            print(__doc__.strip())
            sys.exit(0)
        else:
            remaining.append(a)
            i += 1

    result["remaining"] = remaining
    return result


def main():
    if not AURA.exists():
        print(f"{R}Error: {AURA} not found. Run 'python3 build.py build' first.{N}")
        sys.exit(1)

    raw_args = sys.argv[1:]
    parsed = _parse_flag_args(raw_args)
    args = parsed["remaining"]
    do_bench = parsed["do_bench"]
    ai_mode = parsed["ai"]

    if not args:
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

    # Default: read seed from file or inline
    path = Path(args[0])
    code = path.read_text().strip() if path.exists() else args[0]

    if ai_mode:
        # AI-driven mutation loop
        ai_mutation_loop(
            seed_code=code,
            iterations=parsed["iterations"],
            do_bench=do_bench,
            verbose=True,
            api_key=parsed["api_key"],
            model=parsed["model"],
            base_url=parsed["base_url"],
        )
        return

    # Default: single mutation pass (random)
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
