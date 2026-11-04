"""Embed tpl/<template>/** as src/cli_templates_4913.inc (raw strings)."""

import pathlib
import sys

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
lines = [
    "// Generated from the `aura new` templates (#4913). Edit the templates, not this file.",
    "struct TemplateFile4913 {",
    "    std::string_view tpl;",
    "    std::string_view path;",
    "    std::string_view body;",
    "};",
    "inline constexpr TemplateFile4913 kTemplates4913[] = {",
]
for t in ("app", "evolve", "agent"):
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
out.write_text("\n".join(lines) + "\n")
print(out, len(lines))
