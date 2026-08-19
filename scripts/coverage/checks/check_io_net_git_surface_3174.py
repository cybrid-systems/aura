#!/usr/bin/env python3
"""Issue #3174: demote IO/Net/Git/process prims off core boot.

Core boot add() keeps read-file / write-file / getenv. git/tcp/http/shell/
sys/file-* extras are defer_std_host_prim and installed on (require std/…).
Sandbox without grant-effect refuses the module.

  AC1 Core add() has no git-/tcp-/http-/shell/sys-/file-exists?
  AC2 defer_std_host_prim holds the demoted names; std/git.aura exists
  AC3 ensure_std_host_prims capability gate in load_module_file
  AC4 No new public query key; SlimSurface shrinks
  AC5 Extend sys-open + production-sweep + tcp require suites
  AC6 This linter + build.py; no test_issue_3174.cpp; no docs/design/

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims

ROOT = Path(__file__).resolve().parents[3]

ADD_RE = re.compile(r'add\(\s*"([^"]+)"')
DEFERRED = (
    "git-status",
    "git-diff",
    "git-log",
    "git-commit",
    "git-branch-current",
    "git-stage",
    "git-rev-parse",
    "http-get",
    "http-post",
    "tcp-connect",
    "tcp-listen",
    "tcp-local-port",
    "tcp-accept",
    "tcp-accept-timeout",
    "tcp-send",
    "tcp-recv",
    "tcp-close",
    "sys-open",
    "sys-read",
    "sys-write",
    "shell",
    "command-line",
    "command-output",
    "file-exists?",
    "file-copy",
    "file-delete",
    "file-size",
    "directory-list",
)
KEEP = ("read-file", "write-file", "getenv")


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    io = _read("src/compiler/evaluator_primitives_io.cpp")
    file_cpp = _read("src/compiler/evaluator_primitives_file.cpp")
    loader = _read("src/compiler/evaluator_module_loader.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    git = _read("lib/std/git.aura")
    idx = _read("lib/std/INDEX.aura")
    sys_open = _read("tests/compiler/test_sys_open_path_harden.cpp")
    sweep = _read("tests/compiler/test_production_sweep.cpp")
    cmdline = _read("tests/compiler/test_command_line_cap_io_read.cpp")
    tcp = _read("tests/compiler/test_tcp_listen_accept.cpp")
    build = _read("build.py")
    q = read_query_prims()
    src = io + "\n" + file_cpp
    public = ADD_RE.findall(src)

    for k in KEEP:
        if k not in public:
            fails.append(f"AC1: missing core {k}")
    for name in DEFERRED:
        if re.search(rf'add\(\s*"{re.escape(name)}"', src):
            fails.append(f"AC1: core add() still registers {name}")
        if not re.search(rf'defer_std_host_prim\(\s*"{re.escape(name)}"', src):
            fails.append(f"AC2: deferred body missing {name}")

    must("defer_std_host_prim", "AC2 helper", ixx)
    must("Issue #3174", "AC2 cite", ixx)
    must("ensure_std_host_prims", "AC3 loader", loader)
    must("kEffectNetwork", "AC3 net effect", loader)
    must("kEffectExec", "AC3 process effect", loader)
    must("(define git-status git-status)", "AC2 git alias", git)
    must("std/git", "AC2 INDEX", idx)
    must("std/process", "AC2 INDEX", idx)

    if "query:std-io-net-git" in q or "query:io-net-git-surface" in q:
        fails.append("AC4: new top-level query key (forbidden)")
    must("3174: sys-open unbound before std/process", "AC5 test", sys_open)
    must('ensure_std_host_prims("std/net")', "AC5 sweep net", sweep)
    must('ensure_std_host_prims("std/process")', "AC5 sweep process", sweep)
    must('ensure_std_host_prims("std/process")', "AC5 command-line", cmdline)
    must('ensure_std_host_prims("std/socket")', "AC5 tcp", tcp)
    must("check_io_net_git_surface_3174", "AC6 build", build)
    must("cmd_io_net_git_surface_3174", "AC6 cmd", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3174.cpp").is_file():
        fails.append("AC6: test_issue_3174.cpp present (forbidden #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3174-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print(f"OK: Issue #3174 IO/Net/Git demotion — core keep={list(KEEP)} deferred={len(DEFERRED)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
