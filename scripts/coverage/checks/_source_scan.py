#!/usr/bin/env python3
"""Shared source-shape helpers for the coverage checks.

Issue #3480 follow-up: a fixed character budget (`text[start:start+N]`) rots
whenever the function/lambda/struct it anchors on grows -- #3975-#3988,
#4332, #4385, #4910 each did that and left the assertion reading a slice
that no longer reached its needles. These helpers return the whole
brace-matched block instead, so growth cannot slide the window and a brace
inside a comment or literal cannot cut it short.
"""

from __future__ import annotations


def _skip_literal(text: str, i: int) -> int:
    n = len(text)
    quote = text[i]
    i += 1
    while i < n:
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return n


def _skip_trivia(text: str, j: int) -> int:
    n = len(text)
    while j < n:
        if text.startswith("//", j):
            eol = text.find(chr(10), j)
            j = n if eol < 0 else eol + 1
            continue
        if text.startswith("/*", j):
            end = text.find("*/", j + 2)
            return n if end < 0 else end + 2
        if text[j] in "\"'":
            j = _skip_literal(text, j)
            continue
        break
    return j


def source_block_after(text: str, after: int) -> str:
    """Text from `after` through the end of the first brace block after it.

    Covers function, struct/class and lambda bodies (the first `{` after the
    anchor opens the body). Returns "" when no block opens after `after`.
    """
    n = len(text)
    i = after
    while i < n:
        i = _skip_trivia(text, i)
        if i >= n:
            return ""
        if text[i] == "{":
            break
        i += 1
    start = after
    depth = 0
    while i < n:
        i = _skip_trivia(text, i)
        if i >= n:
            return ""
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return ""


def lambda_body_after(text: str, after: int) -> str:
    """Body of the lambda whose capture list starts at/after `after`."""
    n = len(text)
    i = after
    while i < n:
        i = _skip_trivia(text, i)
        if i >= n:
            return ""
        if text[i] == "[":
            break
        i += 1
    depth = 0
    while i < n:
        i = _skip_trivia(text, i)
        if i >= n:
            return ""
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                i += 1
                break
        i += 1
    return source_block_after(text, i)
