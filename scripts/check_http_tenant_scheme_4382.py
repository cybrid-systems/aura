#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #4382: http-get / http-post accepted file:// and every other
# libcurl scheme (no CURLOPT_PROTOCOLS / CURLOPT_REDIR_PROTOCOLS), and
# never consulted the tenant host-path gate — a Network grant alone read
# /etc/passwd into string_heap_ (or wrote via POST), and FOLLOWLOCATION
# could redirect an http(s) response onto file://. The curl CLI fallback
# received the raw URL as argv with no --proto restriction.
#
# AC1 — Evaluator::check_tenant_http_scheme (evaluator_security.cpp)
#       mirrors check_tenant_exec_jail under the same #3802 policy
#       predicate: active + non-http(s) scheme → IsolationDeny SE
#       (reason tenant-path-escape, kEffectNetwork face, Typed
#       correlate, bump + last_mutate_error), zero perform.
# AC2 — the scheme allowlist predicate (tenant_http_url_scheme_allowed)
#       lives in the tenant_host_path.hh SSOT, cites #4382, and is a
#       case-insensitive http:// / https:// allowlist.
# AC3 — both prim bodies consult the fence BEFORE any perform / async /
#       CLI exec; libcurl paths set CURLOPT_PROTOCOLS +
#       CURLOPT_REDIR_PROTOCOLS to http|https; the curl CLI fallback
#       passes --proto / --proto-redir.
# AC4 — runtime ACs extend the #3802 family host test file
#       (tests/core/test_tenant_isolation_enforcement.cpp); no
#       tests/core/test_issue_4382.cpp, no docs/design/4382-*.
# AC5 — build.py wires this linter and the root allowlist carries it.
#
# Self-test:
#   python3 scripts/check_http_tenant_scheme_4382.py

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _strip_cpp_comments(src: str) -> str:
    out = []
    i, n = 0, len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(src[i])
        i += 1
    return "".join(out)


failures: list[str] = []
passes = 0


def must(cond: bool, label: str) -> None:
    global passes
    if cond:
        passes += 1
    else:
        failures.append(label)


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> int:
    sec_raw = read("src/compiler/evaluator_security.cpp")
    sec = _strip_cpp_comments(sec_raw)

    # ── AC1: the Evaluator scheme fence ──
    must("Issue #4382" in sec_raw, "AC1: security TU cites #4382")
    gate = sec.find("bool Evaluator::check_tenant_http_scheme(")
    must(gate != -1, "AC1: check_tenant_http_scheme defined")
    if gate != -1:
        body = sec[gate:]
        body = body[: body.find("\nbool ", 1)] if body.find("\nbool ", 1) != -1 else body
        must("tenant_host_path_policy_active" in body, "AC1: fence reuses the #3802 policy predicate (no second model)")
        must("tenant_http_url_scheme_allowed" in body, "AC1: allowlist predicate consulted")
        must("kTenantPathEscapeReason" in body, "AC1: shared tenant-path-escape reason")
        must("SecurityEventKind::IsolationDeny" in body, "AC1: deny joins the IsolationDeny row")
        must("kEffectNetwork" in body, "AC1: network effect face stamped on the row")
        must("capture_security_correlated_audit" in body, "AC1: Typed correlate joins mid+tenant+fiber+epoch")
        must(
            "bump_capability_denial" in body and "last_mutate_error_" in body,
            "AC1: deny carries last_mutate_error + denial counter",
        )

    # ── AC2: the SSOT allowlist predicate ──
    hh_raw = read("src/compiler/tenant_host_path.hh")
    hh = _strip_cpp_comments(hh_raw)
    must("Issue #4382" in hh_raw, "AC2: scheme SSOT cites #4382")
    must("bool tenant_http_url_scheme_allowed(" in hh, "AC2: allowlist predicate defined")
    pred = hh.find("bool tenant_http_url_scheme_allowed(")
    if pred != -1:
        body = hh[pred:]
        body = body[: body.find("\n[[nodiscard]]", 1)] if body.find("\n[[nodiscard]]", 1) != -1 else body
        # NOTE: search the RAW header for the scheme literals — the comment
        # stripper eats the `//` inside the "http://" string literals.
        must('starts_with_ci(url, "http://")' in hh_raw, "AC2: http:// allowlisted")
        must('starts_with_ci(url, "https://")' in hh_raw, "AC2: https:// allowlisted")
        must("c - 'A' + 'a'" in body, "AC2: case-insensitive fold (RFC 3986 scheme)")

    # ── AC3: prim wiring — fence before perform, protocols restricted ──
    src = read("src/compiler/evaluator_primitives_io.cpp")
    stripped = _strip_cpp_comments(src)
    hg = stripped.find('defer_std_host_prim(\n        "http-get"')
    if hg == -1:
        hg = stripped.find('defer_std_host_prim("http-get"')
    hp = stripped.find('defer_std_host_prim("http-post"')
    tcp = stripped.find('"tcp-connect"')
    must(hg != -1 and hp != -1 and hg < hp, "AC3: http prim bodies located")
    if hg != -1 and hp != -1:
        hg_body = stripped[hg:hp]
        fence_hg = hg_body.find('check_tenant_http_scheme(url, "http-get")')
        must(fence_hg != -1, "AC3: http-get consults the scheme fence")
        curl_url = hg_body.find("CURLOPT_URL")
        must(
            fence_hg != -1 and curl_url != -1 and fence_hg < curl_url,
            "AC3: http-get fences before CURLOPT_URL (zero perform on deny)",
        )
        must(
            hg_body.count("CURLOPT_PROTOCOLS") == 1 and hg_body.count("CURLOPT_REDIR_PROTOCOLS") == 1,
            "AC3: http-get libcurl path sets PROTOCOLS + REDIR_PROTOCOLS",
        )
        hp_body = stripped[hp:tcp] if tcp != -1 else stripped[hp:]
        fence_hp = hp_body.find('check_tenant_http_scheme(curl_url, "http-post")')
        must(fence_hp != -1, "AC3: http-post consults the scheme fence")
        async_idx = hp_body.find("g_http_post_async")
        must(fence_hp != -1 and async_idx != -1 and fence_hp < async_idx, "AC3: http-post fences before the async path")
        must(
            hp_body.count("CURLOPT_PROTOCOLS") == 1 and hp_body.count("CURLOPT_REDIR_PROTOCOLS") == 1,
            "AC3: http-post libcurl path sets PROTOCOLS + REDIR_PROTOCOLS",
        )
        must(
            '"--proto"' in hp_body and '"--proto-redir"' in hp_body,
            "AC3: curl CLI fallback passes --proto / --proto-redir",
        )

    # ── AC4: runtime ACs extend the family host test file ──
    tf = read("tests/core/test_tenant_isolation_enforcement.cpp")
    must("#4382" in tf, "AC4: host test file cites #4382")
    must("check_tenant_http_scheme" in tf, "AC4: fence matrix pinned in ACs")
    must("file:///etc/passwd" in tf, "AC4: file:// deny pinned in ACs")
    must(not (ROOT / "tests/core/test_issue_4382.cpp").exists(), "AC4: no standalone test_issue_4382.cpp (#81934)")
    design = ROOT / "docs" / "design"
    must(not design.exists() or not any(design.glob("4382-*")), "AC4: no docs/design/4382-* (#1655)")

    # ── AC5: wiring ──
    build = read("build.py")
    must("check_http_tenant_scheme_4382" in build, "AC5: build.py wires the #4382 linter")
    allow = read("scripts/coverage/root_check_allowlist.txt")
    must("check_http_tenant_scheme_4382.py" in allow, "AC5: linter on the root_check_allowlist")

    print(f"check_http_tenant_scheme_4382: {passes} checks passed")
    if failures:
        print("FAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
