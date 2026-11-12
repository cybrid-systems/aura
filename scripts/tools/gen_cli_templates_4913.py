"""Embed tpl/<template>/** as src/cli_templates_4913.inc (raw strings).

Usage:
  python3 scripts/tools/gen_cli_templates_4913.py <templates-dir> <out.inc>
  python3 scripts/tools/gen_cli_templates_4913.py <templates-dir> <out.inc> --check
      # exit 1 if <out.inc> differs from what would be generated

The --check mode is what the fast gate runs: the .inc is a build artifact of
templates/new, and a stale one makes `aura new` serve the old template.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

TEMPLATES = ("app", "evolve", "evolve-moo", "agent")


def render(src: pathlib.Path) -> str:
    """The exact text of the embedded template table for src."""
    lines = [
        "// Generated from the `aura new` templates (#4913). Edit the templates, not this file.",
        "struct TemplateFile4913 {",
        "    std::string_view tpl;",
        "    std::string_view path;",
        "    std::string_view body;",
        "};",
        "inline constexpr TemplateFile4913 kTemplates4913[] = {",
    ]
    for t in TEMPLATES:
        for f in sorted((src / t).rglob("*")):
            if not f.is_file() or f.name in ("run.sh",) or "out" in f.relative_to(src / t).parts:
                continue
            rel = f.relative_to(src / t).as_posix()
            if rel == "ci.yml":
                rel = ".github/workflows/ci.yml"
            body = f.read_text()
            assert ')AURA"' not in body
            lines.append(f'    {{"{t}", "{rel}", R"AURA({body})AURA"}},')
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src", help="templates root, e.g. templates/new")
    ap.add_argument("out", help="generated .inc path")
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if out differs from what would be generated",
    )
    args = ap.parse_args()

    src = pathlib.Path(args.src)
    out = pathlib.Path(args.out)
    content = render(src)

    if args.check:
        current = out.read_text(encoding="utf-8") if out.exists() else None
        if current != content:
            print(f"Embedded CLI templates are stale: {out}", file=sys.stderr)
            print(
                f"Run: python3 scripts/tools/gen_cli_templates_4913.py {args.src} {args.out}",
                file=sys.stderr,
            )
            return 1
        print(f"Embedded CLI templates OK ({out})")
        return 0

    out.write_text(content, encoding="utf-8")
    print(out, len(content.splitlines()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
