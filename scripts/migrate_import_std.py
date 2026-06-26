#!/usr/bin/env python3
"""Migrate aura sources toward import std; and std::print/println."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Standard library headers covered by `import std;` (C++26).
STD_HEADERS = {
    "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset",
    "charconv", "chrono", "codecvt", "compare", "complex", "concepts",
    "condition_variable", "coroutine", "deque", "exception", "execution",
    "expected", "filesystem", "format", "forward_list", "fstream",
    "functional", "future", "generator", "initializer_list", "iomanip",
    "ios", "iosfwd", "iostream", "istream", "iterator", "latch",
    "limits", "list", "locale", "map", "mdspan", "memory", "memory_resource",
    "mutex", "new", "numbers", "numeric", "optional", "ostream", "print",
    "queue", "random", "ranges", "ratio", "regex", "scoped_allocator",
    "semaphore", "set", "shared_mutex", "source_location", "span",
    "sstream", "stack", "stdexcept", "stop_token", "streambuf", "string",
    "string_view", "syncstream", "system_error", "thread", "tuple",
    "type_traits", "typeindex", "typeinfo", "unordered_map", "unordered_set",
    "utility", "valarray", "variant", "vector", "version",
    # C compatibility headers also exported by std module
    "cassert", "cctype", "cerrno", "cfenv", "cfloat", "cinttypes",
    "climits", "clocale", "cmath", "csetjmp", "csignal", "cstdarg",
    "cstddef", "cstdint", "cstdio", "cstdlib", "cstring", "ctime",
    "cwchar", "cwctype",
}

SKIP_PATH_PARTS = {
    "reflect/",
    "linenoise/",
    "lib/",
    "import_std_demo/",
}

SKIP_FILES = {
    "ir_reflect_serialize.cpp",  # reflect.hh + import std ICE risk
    "cache_reflect.cpp",
    "reflect.ixx",
    "linenoise.c",
    "runtime.c",
}

INCLUDE_RE = re.compile(r"^#include\s*<([^>]+)>\s*(?://.*)?$")
MODULE_RE = re.compile(r"^(export\s+)?module\b")
IMPORT_STD_RE = re.compile(r"^import\s+std\s*;")

# std::cout << "text\n"  or  std::cout << expr << "more\n"
COUT_SIMPLE_RE = re.compile(
    r"std::cout\s*<<\s*\"((?:[^\"\\]|\\.)*)\"(?:\s*<<\s*std::endl)?\s*;"
)
COUT_FLUSH_RE = re.compile(
    r"std::cout\s*<<\s*((?:\"(?:[^\"\\]|\\.)*\"|[^;]+?))\s*;\s*std::cout\.flush\(\)\s*;"
)
CERR_STREAM_RE = re.compile(
    r"std::cerr\s*<<\s*(.+?)\s*<<\s*std::endl\s*;",
    re.DOTALL,
)


def should_process(path: Path) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    if path.suffix not in {".cpp", ".cppm", ".ixx", ".hpp"}:
        return False
    if path.name in SKIP_FILES:
        return False
    return not any(part in rel for part in SKIP_PATH_PARTS)


def is_std_include(line: str) -> bool:
    m = INCLUDE_RE.match(line.strip())
    if not m:
        return False
    header = m.group(1)
    return header in STD_HEADERS


def remove_std_includes(lines: list[str]) -> list[str]:
    return [ln for ln in lines if not is_std_include(ln)]


def has_import_std(lines: list[str]) -> bool:
    return any(IMPORT_STD_RE.match(ln.strip()) for ln in lines)


def insert_import_std(lines: list[str]) -> list[str]:
    if has_import_std(lines):
        return lines

    # After `export module X;` or `module X;` (implementation unit)
    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if MODULE_RE.match(stripped) and stripped.endswith(";"):
            out = lines[: i + 1]
            if out and out[-1].strip():
                out.append("")
            out.append("import std;")
            out.extend(lines[i + 1 :])
            return out

    # After initial #include block / module; fragment for non-module TUs
    insert_at = 0
    in_module_fragment = False
    for i, ln in enumerate(lines):
        stripped = ln.strip()
        if stripped == "module;":
            in_module_fragment = True
            insert_at = i + 1
            continue
        if in_module_fragment:
            if stripped.startswith("#include") or stripped == "" or stripped.startswith("//"):
                insert_at = i + 1
                continue
            break
        if stripped.startswith("#include") or stripped == "" or stripped.startswith("//"):
            insert_at = i + 1
            continue
        break

    out = lines[:insert_at]
    if out and out[-1].strip():
        out.append("")
    out.append("import std;")
    out.extend(lines[insert_at:])
    return out


def replace_cout(lines: list[str]) -> list[str]:
    result: list[str] = []
    for ln in lines:
        # flush pattern first
        m = COUT_FLUSH_RE.search(ln)
        if m:
            expr = m.group(1).strip()
            if expr.startswith('"') and expr.endswith('"'):
                inner = expr[1:-1].replace("\\n", "\n").replace("\\t", "\t")
                if inner.endswith("\n"):
                    ln = COUT_FLUSH_RE.sub(f'std::println("{inner[:-1]}");', ln)
                else:
                    ln = COUT_FLUSH_RE.sub(f'std::print("{inner}"); std::cout.flush();', ln)
            else:
                ln = COUT_FLUSH_RE.sub(
                    f"std::print({{}}); std::cout.flush();".replace("{}", expr), ln
                )
        # simple string literal
        ln = COUT_SIMPLE_RE.sub(
            lambda m: (
                f'std::println("{m.group(1).replace(chr(92)+"n", chr(10)).rstrip(chr(10))}");'
                if m.group(1).endswith("\\n")
                else f'std::print("{m.group(1)}");'
            ),
            ln,
        )
        # std::cerr << ... << std::endl
        m = CERR_STREAM_RE.search(ln)
        if m:
            expr = m.group(1).strip()
            # split chained << for simple cases
            parts = [p.strip() for p in re.split(r"\s*<<\s*", expr)]
            if len(parts) == 1:
                ln = CERR_STREAM_RE.sub("std::println(std::cerr, {});".format(parts[0]), ln)
            else:
                # build format string heuristically
                fmt_parts = []
                args = []
                for p in parts:
                    if p.startswith('"') and p.endswith('"'):
                        fmt_parts.append(p[1:-1].replace("{", "{{").replace("}", "}}"))
                    else:
                        fmt_parts.append("{}")
                        args.append(p)
                fmt = "".join(fmt_parts).rstrip("\n")
                if fmt.endswith("\\n"):
                    fmt = fmt[:-2]
                    suffix = ");"
                    call = "std::println(std::cerr, "
                else:
                    suffix = ");"
                    call = "std::print(std::cerr, "
                if args:
                    ln = CERR_STREAM_RE.sub(
                        f"{call}\"{fmt}\", {', '.join(args)}{suffix}", ln
                    )
                else:
                    ln = CERR_STREAM_RE.sub(f"std::println(std::cerr, \"{fmt}\");", ln)
        result.append(ln)
    return result


def process_file(path: Path, *, add_import: bool = True) -> bool:
    original = path.read_text(encoding="utf-8")
    lines = original.splitlines(keepends=True)
    if not lines:
        return False

    changed = False
    had_import = has_import_std(lines)

    if add_import and not had_import:
        new_lines = insert_import_std([ln.rstrip("\n") for ln in lines])
        if new_lines != [ln.rstrip("\n") for ln in lines]:
            changed = True
            lines = [ln + "\n" for ln in new_lines]

    if had_import or (add_import and changed):
        stripped = [ln.rstrip("\n") for ln in lines]
        cleaned = remove_std_includes(stripped)
        if cleaned != stripped:
            changed = True
            lines = [ln + "\n" for ln in cleaned]

    text = "".join(lines)
    new_text = "\n".join(replace_cout(text.splitlines())) + (
        "\n" if original.endswith("\n") else ""
    )
    if new_text != original:
        changed = True

    if changed:
        path.write_text(new_text, encoding="utf-8")
    return changed


def main() -> int:
    targets: list[Path] = []
    for base in (ROOT / "src", ROOT / "tests"):
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if should_process(path):
                targets.append(path)

    changed_files: list[str] = []
    for path in sorted(targets):
        if process_file(path):
            changed_files.append(path.relative_to(ROOT).as_posix())

    print(f"Updated {len(changed_files)} files")
    for f in changed_files:
        print(f"  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())