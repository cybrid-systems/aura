#!/usr/bin/env python3
"""Aura Stdlib Inliner — inline stdlib modules for IR-compatible code.

When code uses (require std/list) or (import "std/list"), the IR pipeline
falls back to tree-walker because import has env-binding side effects.
This inliner preprocesses the code to inline stdlib function definitions
directly, so all code runs through the IR pipeline.

Usage:
  from stdlib_inliner import inline_stdlib
  code = inline_stdlib('(import "std/list")(sort (list 3 1 2))')
  # → '(define (sort lst) ...)...(sort (list 3 1 2))'
"""

import os
import sys

AURA_LIB = os.environ.get("AURA_PATH",
    os.path.join(os.path.dirname(__file__) or ".", "..", "lib"))


def _stdlib_path(module_name: str) -> str | None:
    if module_name.endswith(".aura"):
        module_name = module_name[:-5]
    candidates = []
    if module_name.startswith("std/"):
        candidates.append(os.path.join(AURA_LIB, "std", module_name[4:] + ".aura"))
    else:
        candidates.append(os.path.join(AURA_LIB, "std", module_name + ".aura"))
        candidates.append(os.path.join(AURA_LIB, module_name + ".aura"))
    for c in candidates:
        if os.path.isfile(c):
            return os.path.realpath(c)
    return None


def _extract_defines(filepath: str) -> str:
    """Extract top-level (define ...) forms from a stdlib file."""
    with open(filepath) as f:
        content = f.read()
    defines = []
    depth = 0
    start = 0
    in_string = False
    for i, c in enumerate(content):
        if in_string:
            if c == '\\' and i + 1 < len(content):
                continue
            if c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
            continue
        if c == ';':  # line comment
            while i < len(content) and content[i] != '\n':
                i += 1
            continue
        if c in ('(', '['):
            if depth == 0:
                start = i
            depth += 1
            continue
        if c in (')', ']'):
            depth -= 1
            if depth == 0:
                expr = content[start:i + 1].strip()
                if expr.startswith("(define"):
                    defines.append(expr)
    return "\n".join(defines)


def _find_top_exprs(code: str):
    """Yield (start, end, text) for each top-level s-expression."""
    depth = 0
    start = 0
    in_string = False
    i = 0
    while i < len(code):
        c = code[i]
        if in_string:
            if c == '\\' and i + 1 < len(code):
                i += 1
            elif c == '"':
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            i += 1
            continue
        if c == ';':  # line comment
            while i < len(code) and code[i] != '\n':
                i += 1
            continue
        if c in ('(', '['):
            if depth == 0:
                start = i
            depth += 1
            i += 1
            continue
        if c in (')', ']'):
            depth -= 1
            if depth == 0:
                yield (start, i + 1, code[start:i + 1])
            i += 1
            continue
        i += 1


def _parse_import(expr: str) -> tuple[str, str] | None:
    """Parse a top-level s-expr as import/require. Returns (module, prefix) or None."""
    s = expr.strip()
    if not (s.startswith("(require ") or s.startswith("(import ")):
        return None
    # Strip outer parens
    inner = s[1:-1].strip()
    parts = []
    depth = 0
    current = ""
    in_str = False
    for c in inner:
        if in_str:
            current += c
            if c == '\\':
                continue
            if c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
            current += c
            continue
        if c in ('(', '['):
            depth += 1
        if c in (')', ']'):
            depth -= 1
        if c.isspace() and depth == 0:
            if current:
                parts.append(current)
                current = ""
            continue
        current += c
    if current:
        parts.append(current)

    if len(parts) < 2:
        return None

    keyword = parts[0]  # require or import
    module_ref = parts[1]
    prefix = parts[2] if len(parts) > 2 else ""

    # Extract module name from "path" or std/name
    if module_ref.startswith('"') and module_ref.endswith('"'):
        module_name = module_ref[1:-1]
    elif module_ref.startswith("std/"):
        module_name = module_ref[4:]
    else:
        module_name = module_ref

    return (module_name, prefix)


def inline_stdlib(code: str) -> str:
    """Inline stdlib definitions into code, removing require/import forms."""
    result_parts = []
    inlined_names = set()

    for start, end, expr in _find_top_exprs(code):
        parsed = _parse_import(expr)
        if parsed is not None:
            module_name, prefix = parsed
            path = _stdlib_path(module_name)
            if path:
                defines = _extract_defines(path)
                if defines:
                    result_parts.append(defines)
                    continue  # skip the import line
        # Not an import, or couldn't resolve — keep original
        result_parts.append(expr)

    return "\n".join(result_parts)


def main():
    code = sys.stdin.read()
    print(inline_stdlib(code))


if __name__ == "__main__":
    main()
