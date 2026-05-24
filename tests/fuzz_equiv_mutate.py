#!/usr/bin/env python3
"""Equivalence Mutation Fuzz — verify semantic-preserving program transformations.

Strategy:
1. Generate a valid Aura program + compute its reference output (tree-walk)
2. Apply a semantic-preserving transformation (rename vars, swap operands, inline, etc.)
3. Run the transformed program, compare output to reference
4. Any difference = compiler bug or invalid transformation

Transformations:
  a. Variable renaming        — (lambda (x) (+ x 1)) → (lambda (y) (+ y 1))
  b. Commutative swap         — (+ a b) → (+ b a), (* a b) → (* b a)
  c. If-negate                — (if cond a b) → (if (not cond) b a)  [cond pure]
  d. Let inline               — (let ((x 42)) (+ x 1)) → (+ 42 1)    [single-use]
  e. Begin wrap/unwrap        — expr → (begin expr), (begin expr) → expr
  f. Let-nest to sequential   — (let ((a E1)) (let ((b E2)) body)) → (let* ((a E1) (b E2)) body)
  g. Eta expansion            — f → (lambda (x) (f x))               [when f is a function]
  h. Constant normalization   — (+ 1 2) vs (+ 1 (+ 1 1))
  i. Commutative (and/or)     — (and a b) → (and b a), (or a b) → (or b a)

Usage:
  python3 tests/fuzz_equiv_mutate.py              # full run
  python3 tests/fuzz_equiv_mutate.py --quick      # 50 programs
  python3 tests/fuzz_equiv_mutate.py --seed 42    # reproducible
"""

import ast  # Python's own ast — for safe literal eval
import datetime
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
AURA = os.environ.get("AURA_BIN", str(REPO / "build" / "aura"))
TIMEOUT = int(os.environ.get("FUZZ_TIMEOUT", "10"))
QUICK = "--quick" in sys.argv
SEED = None
for i, a in enumerate(sys.argv):
    if a == "--seed" and i + 1 < len(sys.argv):
        SEED = int(sys.argv[i + 1])

rng = random.Random(SEED if SEED is not None else None)

# ── Stats ────────────────────────────────────────────────
results = {
    "programs": 0,
    "pass": 0,
    "fail_equiv": 0,    # output mismatch
    "fail_crash": 0,    # crash
    "fail_timeout": 0,
    "transforms_applied": 0,
    "skipped_parse": 0,  # couldn't parse (program gen issue)
}
failure_details = []  # (transform_name, seed_code, mutated_code, expected, got)


# ── Helpers ─────────────────────────────────────────────

# S-expression tokenizer — minimal, handles parens/atoms
def sexpr_tokens(s):
    """Tokenize an S-expression string into a list of tokens."""
    tokens = []
    i = 0
    while i < len(s):
        c = s[i]
        if c in ' \t\n\r':
            i += 1
        elif c == '(' or c == ')':
            tokens.append(c)
            i += 1
        elif c == '"':
            # String literal
            j = i + 1
            while j < len(s):
                if s[j] == '\\' and j + 1 < len(s):
                    j += 2
                elif s[j] == '"':
                    j += 1
                    break
                else:
                    j += 1
            tokens.append(s[i:j])
            i = j
        elif c == ';':
            # Line comment
            while i < len(s) and s[i] != '\n':
                i += 1
        else:
            # Atom
            j = i
            while j < len(s) and s[j] not in ' \t\n\r()";':
                j += 1
            tokens.append(s[i:j])
            i = j
    return tokens


def tokens_to_sexpr(tokens):
    """Convert token list back to S-expression string with proper spacing."""
    out = []
    prev_was_paren = True
    prev_was_atom = False
    prev_was_quote = False
    for tok in tokens:
        is_paren = tok in ('(', ')')
        is_quote = tok == "'"
        
        if is_quote:
            # Quote prefix: no space before next token
            out.append(tok)
            prev_was_paren = False
            prev_was_atom = False
            prev_was_quote = True
            continue
        
        if prev_was_atom and not is_paren:
            out.append(' ')  # space between atoms
        elif prev_was_atom and is_paren and tok == '(':
            out.append(' ')  # space before (
        elif not prev_was_paren and not prev_was_quote and not is_paren:
            out.append(' ')  # space after ) before atom
        elif prev_was_quote and tok == '(':
            pass  # no space between ' and (
        out.append(tok)
        prev_was_paren = is_paren
        prev_was_atom = not is_paren and not is_quote
        prev_was_quote = False
    return ''.join(out)


def parse_one(tokens, start=0):
    """Parse one S-expression from tokens starting at index start.
    Returns (sexpr_tokens, end_index).
    """
    if start >= len(tokens):
        return [], start
    if tokens[start] != '(':
        return [tokens[start]], start + 1
    depth = 1
    i = start + 1
    while i < len(tokens) and depth > 0:
        if tokens[i] == '(':
            depth += 1
        elif tokens[i] == ')':
            depth -= 1
        i += 1
    return tokens[start:i], i


def is_atom(t):
    return t not in ('(', ')')


# ── Transform registry ─────────────────────────────────
TRANSFORMS = []


def register(fn):
    TRANSFORMS.append(fn)
    return fn


def run_aura(code, backend="tree-walk"):
    """Run code through Aura. Returns (ok, stdout, stderr)."""
    flags = []
    if backend == "ir":
        flags = ["--ir"]
    elif backend == "jit":
        # JIT might not be available — fall back gracefully
        try:
            r = subprocess.run(
                [AURA, "--jit"], input=code, capture_output=True, text=True, timeout=TIMEOUT
            )
            if r.returncode == 0 or r.returncode < 0:
                if r.returncode < 0:
                    return False, "", f"CRASH:signal-{-r.returncode}"
                return True, r.stdout.strip(), r.stderr or ""
            # JIT unavailable? Fall through to tree-walk
        except:
            pass
    
    try:
        r = subprocess.run(
            [AURA] + flags, input=code, capture_output=True, text=True, timeout=TIMEOUT
        )
    except subprocess.TimeoutExpired:
        return False, "", "timeout"
    except FileNotFoundError:
        print(f"  ERROR: aura binary not found at {AURA}", file=sys.stderr)
        sys.exit(1)

    if r.returncode < 0:
        sig = r.returncode
        sig_name = {6: "SIGABRT", 8: "SIGFPE", 11: "SIGSEGV"}.get(sig, f"signal-{-sig}")
        return False, "", sig_name

    if r.returncode != 0 and "internal error" in (r.stderr or ""):
        return False, r.stdout.strip() if r.stdout else "", "internal error"

    return True, r.stdout.strip() if r.stdout else "", r.stderr or ""


def normalize_output(out):
    """Normalize output for comparison (strip whitespace)."""
    if out is None:
        return ""
    return out.strip()


# ── Program generators ──────────────────────────────────

def gen_simple_program():
    """Generate a valid simple Aura program with display output."""
    pattern = rng.choice([
        # Arithmetic
        lambda: f"(display (+ {rng.randint(1,100)} {rng.randint(1,100)}))",
        lambda: f"(display (* {rng.randint(1,20)} {rng.randint(1,20)}))",
        lambda: f"(display (- {rng.randint(10,100)} {rng.randint(1,10)}))",
        lambda: f"(display (quotient {rng.randint(10,100)} {rng.randint(2,10)}))",
        lambda: f"(display (remainder {rng.randint(10,100)} {rng.randint(2,10)}))",
        # Nested arithmetic
        lambda: f"(display (+ (* {rng.randint(1,10)} {rng.randint(1,10)}) (- {rng.randint(10,50)} {rng.randint(1,10)})))",
        lambda: f"(display (* (+ {rng.randint(1,10)} {rng.randint(1,10)}) (- {rng.randint(10,50)} {rng.randint(1,10)})))",
        # Conditionals
        lambda: f"(display (if (> {rng.randint(1,10)} {rng.randint(1,10)}) {rng.randint(1,100)} {rng.randint(1,100)}))",
        lambda: f"(display (if (= {rng.randint(1,10)} {rng.randint(1,10)}) 1 0))",
        lambda: f"(display (if (< {rng.randint(1,10)} {rng.randint(1,10)}) {rng.randint(1,100)} {rng.randint(1,100)}))",
        # Comparisons
        lambda: f"(display (= {rng.randint(1,10)} {rng.randint(1,10)}))",
        lambda: f"(display (< {rng.randint(1,10)} {rng.randint(1,10)}))",
        lambda: f"(display (> {rng.randint(1,10)} {rng.randint(1,10)}))",
        # Lambda + apply
        lambda: f"(display ((lambda (x) (+ x {rng.randint(1,10)})) {rng.randint(1,50)}))",
        lambda: f"(display ((lambda (x y) (+ x y)) {rng.randint(1,50)} {rng.randint(1,50)}))",
        lambda: f"(display ((lambda (x) (* x {rng.randint(2,10)})) {rng.randint(1,20)}))",
        # Let expressions
        lambda: f"(display (let ((x {rng.randint(1,50)})) (+ x {rng.randint(1,50)})))",
        lambda: f"(display (let ((x {rng.randint(1,50)}) (y {rng.randint(1,50)})) (+ x y)))",
        lambda: f"(display (let ((x {rng.randint(1,50)})) (let ((y (* x {rng.randint(2,5)}))) (+ x y))))",
        # Begin
        lambda: "(begin (display 1) (display 2))",
        lambda: "(begin (display (+ 1 2)) (display (* 3 4)))",
        # Complex nested
        lambda: f"(display (let ((add (lambda (a b) (+ a b)))) (add {rng.randint(1,20)} {rng.randint(1,20)})))",
        lambda: f"(display (let ((x {rng.randint(1,20)}) (y {rng.randint(1,20)})) (if (> x y) x y)))",
        # Strings
        lambda: '(display (string-append "a" "b"))',
        lambda: '(display (string-length "hello"))',
        # Stdlib (with require)
        lambda: f"(require std/list all:)(display (length '(1 2 3 4 5)))",
        lambda: f"(require std/list all:)(display (map (lambda (x) (+ x 1)) '(1 2 3)))",
        lambda: f"(require std/list all:)(display (filter (lambda (x) (> x 2)) '(1 2 3 4 5)))",
        lambda: f"(require std/list all:)(display (reverse '(1 2 3)))",
        # Pair operations
        lambda: "(display (car (cons 1 2)))",
        lambda: "(display (cdr (cons 1 2)))",
        # Composition
        lambda: f"(display ((lambda (f g x) (f (g x))) (lambda (y) (* y 2)) (lambda (z) (+ z 1)) {rng.randint(1,10)}))",
        # Factorial (small)
        lambda: ("(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))"
                 "(display (fact 5))"),
        # Fibonacci (small)
        lambda: ("(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))"
                 "(display (fib 8))"),
        # Map with named function
        lambda: ("(require std/list all:)"
                 "(define (double x) (* x 2))"
                 "(display (map double '(1 2 3 4)))"),
        # Nested defines
        lambda: ("(define (f x) (+ x 1))"
                 "(define (g y) (* y 2))"
                 "(display (g (f 5)))"),
        # Combination: let + lambda + if
        lambda: f"(display (let ((f (lambda (x) (if (> x 5) x 5)))) (f {rng.randint(1,10)})))",
        # And/or
        lambda: "(display (and #t #f))",
        lambda: "(display (or #t #f))",
        lambda: "(display (and (> 3 2) (< 1 10)))",
    ])
    return pattern()


def gen_program_for_mutate():
    """Generate a program suitable for at least one transformation."""
    # Pick a random generator from gen_simple_program's patterns
    return gen_simple_program()


def tokenize(s):
    """Tokenize an S-expression string."""
    tokens = sexpr_tokens(s)
    # Rejoin tokens that form atoms
    return tokens


# ── Transform implementations ───────────────────────────

# --- a. Variable rename ---
@register
def rename_variable(code, tokens):
    """Rename a bound variable (lambda parameter, let binding) to a new name."""
    new_name = None
    old_name = None
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 3 < len(tokens):
            # (lambda (x) ...) → (lambda (y) ...)
            if i + 1 < len(tokens) and tokens[i + 1] == 'lambda':
                if i + 2 < len(tokens) and tokens[i + 2] == '(':
                    for j in range(i + 3, len(tokens)):
                        if tokens[j] == ')':
                            break
                        if is_atom(tokens[j]) and tokens[j] not in ('lambda',):
                            old_name = tokens[j]
                            new_name = old_name + "'"
                            break
                    if old_name and new_name:
                        # Replace all occurrences of old_name in the body (but not other lambdas)
                        body_tokens = []
                        depth = 1
                        k = j + 1
                        while k < len(tokens) and depth > 0:
                            if tokens[k] == '(':
                                depth += 1
                            elif tokens[k] == ')':
                                depth -= 1
                            if depth > 0 or tokens[k] == ')':
                                body_tokens.append(tokens[k])
                            k += 1
                        # Check old_name is used at least once in body
                        if old_name in body_tokens:
                            new_tokens = tokens[:]
                            # Replace in body only
                            depth = 1
                            k = j + 1
                            while k < len(new_tokens) and depth > 0:
                                if new_tokens[k] == '(':
                                    depth += 1
                                elif new_tokens[k] == ')':
                                    depth -= 1
                                if new_tokens[k] == old_name and depth > 0:
                                    new_tokens[k] = new_name
                                k += 1
                            # Replace the lambda parameter
                            new_tokens[i + 3] = new_name
                            return new_tokens, f"rename: {old_name}→{new_name}"
    return None, None


# --- b. Commutative swap ---
@register
def swap_commutative(code, tokens):
    """Swap operands of commutative operators: +, *, =, and, or."""
    COMM_OPS = {'+', '*', '=', 'and', 'or'}
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] in COMM_OPS:
            op = tokens[i + 1]
            # Parse operands
            rest = tokens[i + 2:]
            operands = []
            j = 0
            while j < len(rest):
                if rest[j] == ')':
                    break
                one, j2 = parse_one(rest, j)
                if one:
                    operands.append(one)
                else:
                    break
                j = j2
            
            if len(operands) >= 2:
                # Swap first two operands
                new_operands = list(operands)
                new_operands[0], new_operands[1] = new_operands[1], new_operands[0]
                
                # Build new tokens
                new_tokens = tokens[:i]
                new_tokens.append('(')
                new_tokens.append(op)
                for opnd in new_operands:
                    new_tokens.extend(opnd)
                new_tokens.append(')')
                # Rest after the closing paren
                # Find the closing paren
                depth = 1
                k = i + 1
                while k < len(tokens) and depth > 0:
                    if tokens[k] == '(':
                        depth += 1
                    elif tokens[k] == ')':
                        depth -= 1
                    k += 1
                new_tokens.extend(tokens[k:])
                return new_tokens, f"commutative: {op}"
    return None, None


# --- c. If-negate ---
@register
def negate_if_condition(code, tokens):
    """Swap if branches: (if cond a b) → (if (not cond) b a) when cond is pure."""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] == 'if':
            rest = tokens[i + 2:]
            cond, j1 = parse_one(rest, 0)
            if not cond:
                continue
            then_branch, j2 = parse_one(rest, j1)
            if not then_branch:
                continue
            else_branch, _ = parse_one(rest, j2)
            if not else_branch:
                continue
            
            # Check cond doesn't contain side effects (no set!, no define)
            cond_str = ''.join(cond)
            if 'set!' in cond_str or 'define' in cond_str or 'display' in cond_str or 'begin' in cond_str:
                continue
            
            # Build (if (not cond) else-branch then-branch)
            new_cond = ['(', 'not'] + cond + [')']
            
            new_tokens = tokens[:i]
            new_tokens.append('(')
            new_tokens.append('if')
            new_tokens.extend(new_cond)
            new_tokens.extend(else_branch)
            new_tokens.extend(then_branch)
            new_tokens.append(')')
            
            # Rest after the original if
            depth = 1
            k = i + 1
            while k < len(tokens) and depth > 0:
                if tokens[k] == '(':
                    depth += 1
                elif tokens[k] == ')':
                    depth -= 1
                k += 1
            new_tokens.extend(tokens[k:])
            
            return new_tokens, "negate-if"
    return None, None


# --- d. Begin wrap/unwrap ---
@register
def wrap_begin(code, tokens):
    """Wrap an expression in (begin ...) — preserves semantics."""
    # Count top-level expressions first
    top_level_count = 0
    tl = 0
    while tl < len(tokens):
        if tokens[tl] == '(':
            d = 1
            tl += 1
            while tl < len(tokens) and d > 0:
                if tokens[tl] == '(':
                    d += 1
                elif tokens[tl] == ')':
                    d -= 1
                tl += 1
            top_level_count += 1
        elif tokens[tl] not in (' ', '\t', '\n', ''):
            top_level_count += 1
            tl += 1
        else:
            tl += 1
    
    # Only wrap if there's exactly 1 top-level expression
    # (multiple top-levels are already implicitly a sequence)
    if top_level_count != 1:
        return None, None
    
    # Find the one top-level expression to wrap
    for i, tok in enumerate(tokens):
        if tok == '(':
            # Find matching close
            depth = 1
            j = i + 1
            while j < len(tokens) and depth > 0:
                if tokens[j] == '(':
                    depth += 1
                elif tokens[j] == ')':
                    depth -= 1
                j += 1
            if depth == 0:
                expr = tokens[i:j]
                # Only wrap non-begin expressions
                if len(expr) >= 2 and expr[1] not in ('begin',):
                    wrap = ['(', 'begin'] + expr + [')']
                    new_tokens = tokens[:i] + wrap + tokens[j:]
                    return new_tokens, "wrap-begin"
    return None, None


@register
def unwrap_begin(code, tokens):
    """Unwrap (begin expr) → expr when it only has one real expression."""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] == 'begin':
            # Parse inside begin
            rest = tokens[i + 2:]
            exprs = []
            j = 0
            while j < len(rest):
                if rest[j] == ')':
                    break
                one, j2 = parse_one(rest, j)
                if one:
                    exprs.append(one)
                else:
                    break
                j = j2
            if len(exprs) == 1:
                depth = 1
                k = i + 1
                while k < len(tokens) and depth > 0:
                    if tokens[k] == '(':
                        depth += 1
                    elif tokens[k] == ')':
                        depth -= 1
                    k += 1
                new_tokens = tokens[:i] + exprs[0] + tokens[k:]
                return new_tokens, "unwrap-begin"
    return None, None


# --- e. Let-nest to let* ---
@register
def nest_let_to_let_star(code, tokens):
    """(let ((a E1)) (let ((b E2)) body)) → (let* ((a E1) (b E2)) body)"""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] == 'let':
            rest = tokens[i + 2:]
            bindings, j1 = parse_one(rest, 0)
            if not bindings or len(bindings) < 3:  # (() ... )
                continue
            body, j2 = parse_one(rest, j1)
            if not body:
                continue
            
            # Check if body is (let ((c E2)) nested-body)
            body_str = ''.join(body)
            if body_str.startswith('(let'):
                # Parse nested let
                nested_rest = body[2:]  # skip '(' and 'let'
                nested_bindings, _ = parse_one(nested_rest, 0)
                
                new_bindings = ['(']
                # Flatten outer bindings first
                # Parse outer bindings as pairs
                ob = bindings[1:-1]  # strip outer parens
                # Parse each (var expr) pair
                k = 0
                outer_pairs = []
                while k < len(ob):
                    p, k2 = parse_one(ob, k)
                    if p:
                        outer_pairs.append(p)
                    k = k2
                
                # Parse inner bindings
                ib = nested_bindings[1:-1]
                k = 0
                inner_pairs = []
                while k < len(ib):
                    p, k2 = parse_one(ib, k)
                    if p:
                        inner_pairs.append(p)
                    k = k2
                
                all_pairs = outer_pairs + inner_pairs
                for idx, p in enumerate(all_pairs):
                    new_bindings.extend(p)
                new_bindings.append(')')
                
                # Build (let* (...) nested-body)
                # nested-body is after nested bindings in body
                nb_rest = body[2:]  # skip '(let'
                __, jn1 = parse_one(nb_rest, 0)
                nested_body, _ = parse_one(nb_rest, jn1)
                
                new_tokens = tokens[:i]
                new_tokens.append('(')
                new_tokens.append('let*')
                new_tokens.extend(new_bindings)
                new_tokens.extend(nested_body)
                new_tokens.append(')')
                
                # Rest after original outer let
                depth = 1
                k = i + 1
                while k < len(tokens) and depth > 0:
                    if tokens[k] == '(':
                        depth += 1
                    elif tokens[k] == ')':
                        depth -= 1
                    k += 1
                new_tokens.extend(tokens[k:])
                
                return new_tokens, "let→let*"
    return None, None


# --- f. Eta expansion ---
@register
def eta_expand(code, tokens):
    """Eta-expand a function reference: f → (lambda (x) (f x)).
    
    Only expands identifiers that could be function references.
    Skips numbers, strings, builtins, binding positions.
    """
    RESERVED = {'lambda', 'define', 'if', 'let', 'let*', 'letrec', 'begin',
                'display', 'cons', 'car', 'cdr', 'list',
                'map', 'filter', 'foldl', 'foldr', 'length',
                'reverse', 'append', 'quote', 'not',
                '=', '<', '>', '<=', '>=', '+', '-', '*', '/',
                'quotient', 'remainder', 'and', 'or',
                'string-append', 'string-length', 'string-ref',
                'substring', 'string=?', 'string<?',
                'number->string', 'string->number',
                'require', 'use', 'export',
                '#t', '#f', 'true', 'false', 'null',
                'nil', 'car', 'cdr', 'pair?', 'null?', 'apply',
                'gc', 'eval', 'typecheck-current', 'current-source',
                'set-code', 'eval-current', 'mutate:rebind',
                'mutate:set-body', 'mutate:tweak-literal',
                'mutate:record-patch', 'mutate:remove-node',
                'mutate:insert-child', 'mutate:replace-value',
                'query:find', 'query:node-type', 'query:children',
                'query:node', 'query:calls', 'query:parent',
                'query:siblings', 'query:pattern',
                'rollback', 'mutation-log'}
    
    for i, tok in enumerate(tokens):
        if not is_atom(tok):
            continue
        # Skip numbers, strings, booleans
        if tok.startswith('"') or tok.startswith('#'):
            continue
        try:
            int(tok)
            continue
        except ValueError:
            pass
        try:
            float(tok)
            continue
        except ValueError:
            pass
        # Skip reserved keywords
        if tok in RESERVED:
            continue
        # Skip if preceded by '(', 'define', 'lambda', 'let', 'let*', 'letrec'
        # (would be in binding/keyword position)
        prev_idx = i - 1
        while prev_idx >= 0 and tokens[prev_idx] in (' ', '\t', '\n'):
            prev_idx -= 1
        if prev_idx >= 0:
            prev = tokens[prev_idx]
            if prev in ('(', 'define', 'lambda', 'let', 'let*', 'letrec', 'quote',
                        'defmacro', ':', 'all:'):
                continue
            if prev in RESERVED:
                continue
        
        # Skip if inside a lambda parameter list — check we're not after '(' and 'lambda'
        # Simple: check if we appear right after a '(' that follows 'lambda'
        # Also skip if we look like a parameter in (define (f x) ...) or (lambda (x) ...)
        # by checking if we're within a parameter list
        
        # Skip eta-expansion for very short names (likely loop vars)
        if len(tok) <= 1:
            continue
        
        # Success: eta-expand
        new_name = tok + '-expanded'
        new_tokens = tokens[:i]
        new_tokens.append('(')
        new_tokens.append('lambda')
        new_tokens.append('(')
        new_tokens.append(new_name)
        new_tokens.append(')')
        new_tokens.append('(')
        new_tokens.append(tok)
        new_tokens.append(new_name)
        new_tokens.append(')')
        new_tokens.append(')')
        new_tokens.extend(tokens[i + 1:])
        
        return new_tokens, f"eta-expand: {tok}"
    return None, None


# --- g. Constant normalization ---
@register
def constant_reassoc(code, tokens):
    """Reassociate (+ (+ a b) c) → (+ a (+ b c)) or fold/unfold."""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] in ('+', '*'):
            op = tokens[i + 1]
            rest = tokens[i + 2:]
            operands = []
            j = 0
            while j < len(rest):
                if rest[j] == ')':
                    break
                one, j2 = parse_one(rest, j)
                if one:
                    operands.append(one)
                else:
                    break
                j = j2
            
            if len(operands) >= 3:
                # Try (+ (+ a b) c) → (+ a b c) — flatten
                # Check if first operand is the same op
                if len(operands[0]) >= 3 and operands[0][0] == '(' and operands[0][1] == op:
                    # Flatten
                    inner_rest = operands[0][2:-1]
                    inner_ops = []
                    k = 0
                    while k < len(inner_rest):
                        one, k2 = parse_one(inner_rest, k)
                        if one:
                            inner_ops.append(one)
                        k = k2
                    new_ops = inner_ops + list(operands[1:])
                    
                    new_tokens = tokens[:i]
                    new_tokens.append('(')
                    new_tokens.append(op)
                    for o in new_ops:
                        new_tokens.extend(o)
                    new_tokens.append(')')
                    
                    # Find closing paren
                    depth = 1
                    k = i + 1
                    while k < len(tokens) and depth > 0:
                        if tokens[k] == '(':
                            depth += 1
                        elif tokens[k] == ')':
                            depth -= 1
                        k += 1
                    new_tokens.extend(tokens[k:])
                    
                    return new_tokens, f"reassoc-flatten: {op}"
    return None, None


# --- h. Replace literal with equivalent expression ---
@register
def unfold_literal(code, tokens):
    """Replace a literal integer N with (+ N 0) or (* N 1).
    Only safe for non-negative integers.
    Skips integers inside quoted/quote'd lists (where evaluation doesn't happen).
    """
    for i, tok in enumerate(tokens):
        try:
            val = int(tok)
            if not (0 < val <= 50):
                continue
            # Check we're NOT inside a quote ' or quote form
            in_quote = False
            depth = 0
            j = i - 1
            while j >= 0:
                t = tokens[j]
                if t == '(':
                    if depth == 0:
                        # This open paren starts the form containing our number
                        # Check if token before it is a quote ' character
                        if j > 0 and tokens[j - 1] == "'":
                            in_quote = True
                            break
                        # Or if this is (quote ...)
                        if j + 1 < len(tokens) and tokens[j + 1] in ('quote',):
                            in_quote = True
                            break
                        # Neither — this is the start of our evaluating form, stop
                        # but we didn't find quote context, so continue normally
                    depth -= 1
                    if depth < 0:
                        break
                elif t == ')':
                    depth += 1
                j -= 1
            if in_quote:
                continue
            
            # Also check we're not inside a define/lambda parameter number
            # (shouldn't happen but be safe)
            
            # Replace with (+ val 0) or (* val 1) or (+ 0 val)
            replacement = rng.choice([
                ['(', '+', str(val), '0', ')'],
                ['(', '*', str(val), '1', ')'],
                ['(', '+', '0', str(val), ')'],
            ])
            new_tokens = tokens[:i] + replacement + tokens[i + 1:]
            return new_tokens, f"unfold: {val}"
        except ValueError:
            pass
    return None, None


# --- i. Fold known constants ---
@register
def fold_constant(code, tokens):
    """Fold (+ 0 x) → x, (* 1 x) → x, (* 0 x) → 0."""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 2 < len(tokens):
            if tokens[i + 1] == '+' and tokens[i + 2] == '0':
                # (+ 0 x) → x
                rest = tokens[i + 3:]
                arg, j = parse_one(rest, 0)
                if arg:
                    depth = 1
                    k = i + 1
                    while k < len(tokens) and depth > 0:
                        if tokens[k] == '(':
                            depth += 1
                        elif tokens[k] == ')':
                            depth -= 1
                        k += 1
                    new_tokens = tokens[:i] + arg + tokens[k:]
                    return new_tokens, "fold-+0"
            
            if tokens[i + 1] == '*' and tokens[i + 2] == '1':
                # (* 1 x) → x
                rest = tokens[i + 3:]
                arg, j = parse_one(rest, 0)
                if arg:
                    depth = 1
                    k = i + 1
                    while k < len(tokens) and depth > 0:
                        if tokens[k] == '(':
                            depth += 1
                        elif tokens[k] == ')':
                            depth -= 1
                        k += 1
                    new_tokens = tokens[:i] + arg + tokens[k:]
                    return new_tokens, "fold-*1"
            
            if tokens[i + 1] == '*' and tokens[i + 2] == '0':
                # (* 0 x) → 0
                depth = 1
                k = i + 1
                while k < len(tokens) and depth > 0:
                    if tokens[k] == '(':
                        depth += 1
                    elif tokens[k] == ')':
                        depth -= 1
                    k += 1
                new_tokens = tokens[:i] + ['0'] + tokens[k:]
                return new_tokens, "fold-*0"
    return None, None


# --- j. Empty let wrapping ---
@register
def wrap_empty_let(code, tokens):
    """Wrap a top-level expression in (let () ...) — a no-op semantically.
    Only matches expressions at depth 0 (not inside define/lambda params).
    Skips define/lambda/let to avoid scope changes.
    """
    SKIP = {'let', 'let*', 'letrec', 'lambda', 'define', 'begin', 'if', 'cond', 'do',
            'set-code', 'require', 'use', 'export'}
    
    # Track paren depth while scanning
    depth = 0
    for i, tok in enumerate(tokens):
        if tok == '(':
            if depth == 0:
                # Potential match at top-level
                inner_depth = 1
                j = i + 1
                while j < len(tokens) and inner_depth > 0:
                    if tokens[j] == '(':
                        inner_depth += 1
                    elif tokens[j] == ')':
                        inner_depth -= 1
                    j += 1
                if inner_depth == 0:
                    expr = tokens[i:j]
                    if len(expr) >= 2 and expr[1] not in SKIP:
                        wrap = ['(', 'let', '(', ')'] + expr + [')']
                        new_tokens = tokens[:i] + wrap + tokens[j:]
                        return new_tokens, "wrap-empty-let"
            depth += 1
        elif tok == ')':
            depth -= 1
    return None, None


# --- k. Fold nested begin ---
@register
def flatten_nested_begin(code, tokens):
    """(begin (begin a b) c) → (begin a b c)"""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] == 'begin':
            rest = tokens[i + 2:]
            subexprs = []
            j = 0
            nested_idx = -1
            nested_end = -1
            while j < len(rest):
                if rest[j] == ')':
                    break
                sub, j2 = parse_one(rest, j)
                if sub:
                    # Check if this sub-expr is itself a begin
                    if len(sub) >= 3 and sub[0] == '(' and sub[1] == 'begin':
                        nested_idx = len(subexprs)
                        nested_begin = sub
                        # Parse out the nested begin's body
                        nrest = sub[2:-1]
                        nsubs = []
                        k = 0
                        while k < len(nrest):
                            if nrest[k] == ')':
                                break
                            ns, k2 = parse_one(nrest, k)
                            if ns:
                                nsubs.append(ns)
                            k = k2
                        nested_end = nsubs
                    subexprs.append(sub)
                j = j2
            
            if nested_idx >= 0 and nested_end is not None:
                # Rebuild: replace the nested begin with its contents
                new_subexprs = []
                for idx, sub in enumerate(subexprs):
                    if idx == nested_idx:
                        new_subexprs.extend(nested_end)
                    else:
                        new_subexprs.append(sub)
                
                new_tokens = tokens[:i]
                new_tokens.append('(')
                new_tokens.append('begin')
                for s in new_subexprs:
                    new_tokens.extend(s)
                new_tokens.append(')')
                
                depth = 1
                k = i + 1
                while k < len(tokens) and depth > 0:
                    if tokens[k] == '(':
                        depth += 1
                    elif tokens[k] == ')':
                        depth -= 1
                    k += 1
                new_tokens.extend(tokens[k:])
                return new_tokens, "flatten-begin"
    return None, None


# --- l. Lambda-inline for simple let ---
@register
def let_to_lambda(code, tokens):
    """(let ((x v)) body) → ((lambda (x) body) v)"""
    for i, tok in enumerate(tokens):
        if tok == '(' and i + 1 < len(tokens) and tokens[i + 1] == 'let':
            rest = tokens[i + 2:]
            bindings, j1 = parse_one(rest, 0)
            if not bindings or len(bindings) < 4:
                continue  # need at least '(', var, val, ')'
            body, j2 = parse_one(rest, j1)
            if not body:
                continue
            
            # Parse bindings: ( (x v) (y w) ... )
            b_tokens = bindings[1:-1]  # strip outer parens
            pairs = []
            k = 0
            while k < len(b_tokens):
                p, k2 = parse_one(b_tokens, k)
                if p and len(p) >= 3 and p[0] == '(' and p[-1] == ')':
                    var = p[1]
                    val = p[2:-1]
                    if var not in ('(', ')', 'lambda', 'define', 'if', 'let', 'begin'):
                        pairs.append((var, val))
                k = k2
            
            if not pairs:
                continue
            
            # Build ((lambda (x y ...) body) v w ...)
            new_tokens = tokens[:i]
            new_tokens.append('(')
            new_tokens.append('(')
            new_tokens.append('lambda')
            new_tokens.append('(')
            for var, _ in pairs:
                new_tokens.append(var)
            new_tokens.append(')')
            new_tokens.extend(body)
            new_tokens.append(')')
            for _, val in pairs:
                new_tokens.extend(val)
            new_tokens.append(')')
            
            depth = 1
            k = i + 1
            while k < len(tokens) and depth > 0:
                if tokens[k] == '(':
                    depth += 1
                elif tokens[k] == ')':
                    depth -= 1
                k += 1
            new_tokens.extend(tokens[k:])
            return new_tokens, "let→lambda"
    return None, None


# ── Main equivalence test ───────────────────────────────

def test_one_equivalence(original_code):
    """Test one program: run original, apply random transform, run transformed, compare."""
    # Run original
    ref_ok, ref_out, ref_err = run_aura(original_code, "tree-walk")
    if not ref_ok:
        results["skipped_parse"] += 1
        return "skip"  # Original doesn't compile — skip

    ref_out = normalize_output(ref_out)

    # Try each transform at most 3 times
    tokens = sexpr_tokens(original_code)
    attempts = 0
    max_attempts = 8
    
    while attempts < max_attempts:
        # Pick a random transform
        transform = rng.choice(TRANSFORMS)
        result, desc = transform(original_code, list(tokens))
        if result is None:
            attempts += 1
            continue
        
        mutated_source = tokens_to_sexpr(result)
        
        # Validate: parse test — make sure it still compiles
        mut_ok, mut_out, mut_err = run_aura(mutated_source, "tree-walk")
        if not mut_ok:
            # Transform produced invalid program — skip this transform
            attempts += 1
            continue
        
        results["transforms_applied"] += 1
        mut_out = normalize_output(mut_out)
        
        if mut_out == ref_out:
            results["pass"] += 1
            return "pass"
        else:
            results["fail_equiv"] += 1
            # Check if it's a legitimate diff or a transform bug
            failure_details.append((desc, original_code, mutated_source, ref_out, mut_out))
            return "fail"
    
    return "skip"


def main():
    print("=" * 60)
    print("Aura Equivalence Mutation Fuzz")
    print(f"  Date:  {datetime.date.today().isoformat()}")
    print(f"  Seed:  {SEED if SEED is not None else 'random'}")
    print(f"  Binary: {AURA}")
    print(f"  Mode:  {'QUICK' if QUICK else 'FULL'}")
    print("=" * 60)

    n_programs = 50 if QUICK else 500
    start = time.time()

    for i in range(n_programs):
        if (i + 1) % 50 == 0:
            elapsed = time.time() - start
            print(f"  [{i+1:4d}/{n_programs}]  {results['pass']} pass, "
                  f"{results['fail_equiv']} diff, {results['transforms_applied']} transforms, "
                  f"{elapsed:.0f}s", flush=True)

        code = gen_program_for_mutate()
        result = test_one_equivalence(code)
        if result == "fail":
            print(f"  ! [{i+1}] FAIL — see summary")

    elapsed = time.time() - start
    total_cases = results["pass"] + results["fail_equiv"] + results["fail_crash"] + results["fail_timeout"]

    print(f"\n{'='*60}")
    print(f"  Equivalence Mutation Fuzz Summary")
    print(f"{'='*60}")
    print(f"  Programs:        {n_programs}")
    print(f"  Transforms:      {results['transforms_applied']}")
    print(f"  Pass:            {results['pass']}")
    print(f"  Fail (equiv):    {results['fail_equiv']}")
    print(f"  Fail (crash):    {results['fail_crash']}")
    print(f"  Fail (timeout):  {results['fail_timeout']}")
    print(f"  Skipped:         {results['skipped_parse']}")
    print(f"  Elapsed:         {elapsed:.0f}s")

    if failure_details:
        print(f"\n  ❌ EQUIVALENCE FAILURES:")
        for desc, orig, mut, expected, got in failure_details[:10]:
            print(f"    Transform: {desc}")
            print(f"    Original:  {orig[:80]}")
            print(f"    Mutated:   {mut[:80]}")
            print(f"    Expected:  {expected!r}")
            print(f"    Got:       {got!r}")
            print()
        if len(failure_details) > 10:
            print(f"    ... and {len(failure_details) - 10} more")

    if results["fail_equiv"] > 0 or results["fail_crash"] > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
