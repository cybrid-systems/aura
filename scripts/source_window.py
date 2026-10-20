#!/usr/bin/env python3
"""Structural source windows for source-cite linters (anchor-based, no widths).

A source-cite check asserts on a marker that lives inside one function or one
switch-case body. A fixed byte window (src[start:start + 5200]) silently
loses the marker as soon as the body grows, so the check either false-fails or
has to be widened by hand. These helpers slice by structure instead:

    body_between(text, begin, end) -> (slice, ok)   # [begin, end)
    brace_body(text, sig)          -> (slice, ok)   # signature .. matching '}'
    case_body(text, label)         -> (slice, ok)   # case .. next sibling label

ok is False when an anchor is missing / unmatched. Callers must treat that as
a hard failure: pass the tuple to check() (aggregating linters) or require()
(loud, raises WindowError). Never fall back to a byte width and never fall
back to the rest of the file.
"""

from __future__ import annotations


class WindowError(RuntimeError):
    """A structural anchor is missing -- the check cannot prove its claim."""


def _ident(c: str) -> bool:
    return c.isalnum() or c == "_"


def _token_start(text: str, i: int) -> bool:
    return i == 0 or not _ident(text[i - 1])


def _line_indent_only(text: str, i: int) -> bool:
    p = i
    while p > 0 and text[p - 1] != "\n":
        if text[p - 1] not in " \t\r":
            return False
        p -= 1
    return True


def _skip_literal(text: str, i: int) -> int:
    """If a literal/comment starts at i, return the index just past it."""
    n = len(text)
    if i >= n:
        return i
    if i + 1 < n:
        if text.startswith("//", i):
            e = text.find("\n", i + 2)
            return n if e == -1 else e + 1
        if text.startswith("/*", i):
            e = text.find("*/", i + 2)
            return n if e == -1 else e + 2
        if text.startswith('R"', i):
            op = text.find("(", i + 2)
            if op == -1:
                return n
            delim = text[i + 2 : op]
            p = op + 1
            while True:
                close = text.find(")", p)
                if close == -1:
                    return n
                tail = close + 1 + len(delim)
                if tail < n and text[close + 1 : tail] == delim and text[tail] == '"':
                    return tail + 1
                p = close + 1
    if text[i] in ('"', "'"):
        if text[i] == "'" and i > 0 and _ident(text[i - 1]):
            return i  # digit separator: 1'000
        quote = text[i]
        p = i + 1
        while p < n:
            if text[p] == "\\":
                p += 2
                continue
            if text[p] == quote:
                return p + 1
            p += 1
        return n
    return i


def _depth_at(text: str, pos: int) -> int:
    depth = 0
    i = 0
    while i < pos:
        skip = _skip_literal(text, i)
        if skip != i:
            i = skip
            continue
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return depth


def body_between(text: str, begin: str, end: str) -> tuple[str, bool]:
    """Slice [begin, end); False when either anchor is missing or out of order."""
    b = text.find(begin)
    if b < 0:
        return "", False
    e = text.find(end, b + len(begin))
    if e < 0:
        return "", False
    return text[b:e], True


def brace_body(text: str, sig: str) -> tuple[str, bool]:
    """Slice from sig to its matching closing brace (inclusive)."""
    p = text.find(sig)
    if p < 0:
        return "", False
    i = p + len(sig)
    n = len(text)
    tail = i
    while tail > p and text[tail - 1] in " \t":
        tail -= 1
    if tail > p and text[tail - 1] == "{":
        i = tail - 1  # signature already names the opening brace
    else:
        while i < n:
            skip = _skip_literal(text, i)
            if skip != i:
                i = skip
                continue
            if text[i] == ";":
                return "", False  # declaration without a body
            if text[i] == "{":
                break
            i += 1
    if i >= n:
        return "", False
    depth = 0
    while i < n:
        skip = _skip_literal(text, i)
        if skip != i:
            i = skip
            continue
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[p : i + 1], True
        i += 1
    return "", False


def case_body(text: str, label: str) -> tuple[str, bool]:
    """Slice a case/default label to the next sibling label at the same depth."""
    p = text.find(label)
    while p >= 0:
        if _line_indent_only(text, p) and _token_start(text, p):
            break
        p = text.find(label, p + 1)
    if p < 0:
        return "", False
    base = _depth_at(text, p)
    i = p + len(label)
    n = len(text)
    depth = base
    group_tail = p + len(label)
    while i < n:
        skip = _skip_literal(text, i)
        if skip != i:
            i = skip
            continue
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            if depth == base:
                return text[p:i], True
            depth -= 1
        elif depth == base and _token_start(text, i):
            for word in ("case", "default"):
                if text.startswith(word, i):
                    after = i + len(word)
                    if after >= n or not _ident(text[after]):
                        # Grouped labels (case A: case B: body) share one
                        # body; whitespace-only content continues the group.
                        if text[group_tail:i].strip() == "":
                            colon = text.find(":", i)
                            nl2 = text.find("\n", i)
                            if colon >= 0 and (nl2 < 0 or colon < nl2):
                                group_tail = colon + 1
                                i = group_tail
                            else:
                                i = after
                            continue
                        return text[p:i], True
        i += 1
    return "", False


def check(name: str, result: tuple[str, bool], fails: list[str], detail: str) -> str:
    """Aggregating failure path: append a FAIL row when the window is missing."""
    text, ok = result
    if not ok:
        fails.append(f"{name}: structural window {detail} not found (anchors required)")
        return ""
    return text


def require(name: str, result: tuple[str, bool], detail: str) -> str:
    """Loud failure path: raise WindowError when the window is missing."""
    text, ok = result
    if not ok:
        raise WindowError(f"{name}: structural window {detail} not found (anchors required)")
    return text
