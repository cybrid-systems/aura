// test_issue_473.cpp — Verify Issue #473 Tier 1 security fixes
// (serve-async: JSON escape + stdin cap, scope-limited close).
//
// Scope:
//   §1  Cap stdin line length at 1 MiB (kMaxServeAsyncLineBytes)
//   §5  Escape all U+0000–U+001F control chars in json_escape
//   §7  Hardcoded libcurl.so.4 → multi-platform dlopen fallback (compile-time)
//   §6  git-commit ::system() → fork+execvp w/ explicit argv
//   §8  Auth token not exposed in process /cmdline (header reading test)
//
// The header-based helpers (detail::json_escape, detail::json_field,
// detail::kMaxServeAsyncLineBytes) are exercised directly. The
// curl/libgit2/system() plumbing is validated by symbol-presence
// checks (link-time) plus a small stderr probe.

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

#include "serve/serve_async.h"

#include <dlfcn.h>
#include <fstream>
#include <sstream>

using aura::serve::detail::json_escape;
using aura::serve::detail::json_field;
using aura::serve::detail::kMaxServeAsyncLineBytes;

// Issue #473 §6/§7/§8: read a source file as a string for static-pattern
// checks. The Tier-1 fix lives in two .cpp files (serve_async.cpp and
// evaluator_primitives_io.cpp). We grep for the expected tokens (fork+
// execlp, kSonames entries, CURLOPT_HTTPHEADER use) and assert the absence
// of the pre-fix patterns (::system("git commit"), argv.push_back("Authorization"
// in the http_post_async handler). This is a structural regression guard
// — semantic correctness is verified by manual review and runtime tests.
static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Compile-time path discovery: CMake passes AURA_TEST_SRC_DIR as the
// absolute path to the repo's src/ directory (set in CMakeLists.txt).
// We fall back to "src" if the macro isn't defined (allows the test to
// run from a manually-invoked build too).
#if !defined(AURA_TEST_SRC_DIR)
#define AURA_TEST_SRC_DIR "src"
#endif

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

// Issue #473 §5: std::println's `{}` format for std::string truncates at the
// first NUL byte on some implementations. For comparing strings that may
// contain embedded NULs, print sizes + sizes + first/last few bytes.
namespace detail_t {
template <typename T> std::string size_str(const T& v) {
    if constexpr (requires { v.size(); })
        return std::to_string(v.size());
    else
        return std::string{"<n/a>"};
}
} // namespace detail_t
#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto av = (a);                                                                             \
        auto bv = (b);                                                                             \
        if (av == bv) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {} (sz={})", msg, detail_t::size_str(av));                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println("  FAIL: {} (av.sz={} bv.sz={})", msg, detail_t::size_str(av),            \
                         detail_t::size_str(bv));                                                  \
        }                                                                                          \
    } while (0)

// ── AC1: cap constant exists + is 1 MiB ───────────────────────────────

static bool ac_cap_constant() {
    std::println("\n--- AC1: kMaxServeAsyncLineBytes = 1 MiB ---");
    CHECK_EQ(kMaxServeAsyncLineBytes, std::size_t{1u << 20}, "cap is 1 MiB (1048576 bytes)");
    return g_failed == 0;
}

// ── AC2: §5 control-char escape (RFC 8259 §7) ─────────────────────────

static bool ac_escape_basic() {
    std::println("\n--- AC2: json_escape basics (\\\" \\\\ \\n \\r \\t) ---");
    CHECK_EQ(json_escape("hello"), std::string{"hello"}, "no-op on innocuous string");
    CHECK_EQ(json_escape("\""), std::string{"\\\""}, "quote");
    CHECK_EQ(json_escape("\\"), std::string{"\\\\"}, "backslash");
    CHECK_EQ(json_escape("\n"), std::string{"\\n"}, "newline");
    CHECK_EQ(json_escape("\r"), std::string{"\\r"}, "carriage return");
    CHECK_EQ(json_escape("\t"), std::string{"\\t"}, "tab");
    return g_failed == 0;
}

static bool ac_escape_control() {
    std::println("\n--- AC3: json_escape escapes all U+0000–U+001F ---");
    // NULL byte: \u0000
    CHECK_EQ(json_escape(std::string_view("\0", 1)), std::string{"\\u0000"},
             "NUL (U+0000) escaped as \\u0000");
    // SOH (U+0001) and STX (U+0002)
    CHECK_EQ(json_escape(std::string_view("\x01", 1)), std::string{"\\u0001"},
             "SOH (U+0001) escaped as \\u0001");
    CHECK_EQ(json_escape(std::string_view("\x02", 1)), std::string{"\\u0002"},
             "STX (U+0002) escaped as \\u0002");
    // 0x1F (US = unit separator)
    CHECK_EQ(json_escape(std::string_view("\x1f", 1)), std::string{"\\u001f"},
             "US  (U+001F) escaped as \\u001f");
    // 0x10 (DLE)
    CHECK_EQ(json_escape(std::string_view("\x10", 1)), std::string{"\\u0010"},
             "DLE (U+0010) escaped as \\u0010");
    // Already-handled (tab, newline, etc) keep their original short form
    CHECK_EQ(json_escape(std::string_view("\x09", 1)), std::string{"\\t"},
             "tab keeps short \\t (not \\u0009)");
    CHECK_EQ(json_escape(std::string_view("\x0a", 1)), std::string{"\\n"},
             "newline keeps short \\n");
    CHECK_EQ(json_escape(std::string_view("\x0d", 1)), std::string{"\\r"},
             "carriage return keeps short \\r");
    // 0x20 (space) and beyond: NOT escaped
    CHECK_EQ(json_escape(std::string_view(" ")), std::string{" "}, "space (U+0020) NOT escaped");
    CHECK_EQ(json_escape(std::string_view("A")), std::string{"A"}, "printable ASCII NOT escaped");
    return g_failed == 0;
}

static bool ac_escape_mixed() {
    std::println("\n--- AC4: json_escape mixed string ---");
    // Realistic attack-string: a JSON-injection attempt with NUL + control
    // bytes smuggled inside an ostensibly valid field value.
    // Use explicit-length string (the literal contains a NUL byte so a
    // bare string_view would otherwise truncate at the first NUL).
    const char attack_buf[] = {'s',    'a',    'f', 'e', '\x00', '\x01',
                               '\x02', '\x03', 't', 'e', 'x',    't'};
    std::string_view attack{attack_buf, sizeof(attack_buf)};
    std::string escaped = json_escape(attack);
    // Expected: literal \u0000\u0001\u0002\u0003 (NOT interpreted as
    // universal-character-names — we use raw string to get the literal
    // backslash-u-0-0-0-0 shape from json_escape's output).
    std::string expected = R"(safe\u0000\u0001\u0002\u0003text)";
    CHECK_EQ(escaped, expected, "mixed string escapes each control char");
    return g_failed == 0;
}

// ── AC3: §5 round-trip via json_field ─────────────────────────────────

static bool ac_field_extract() {
    std::println("\n--- AC5: json_field extracts value ---");
    {
        std::string_view j = R"json({"cmd": "exec", "code": "(+ 1 2)"})json";
        CHECK_EQ(json_field(j, "cmd"), std::string{"exec"}, "extract `cmd` (space after colon)");
        CHECK_EQ(json_field(j, "code"), std::string{"(+ 1 2)"}, "extract `code`");
    }
    {
        std::string_view j = R"json({"cmd":"exec","code":"(+ 1 2)"})json";
        CHECK_EQ(json_field(j, "cmd"), std::string{"exec"}, "extract `cmd` (no space)");
        CHECK_EQ(json_field(j, "code"), std::string{"(+ 1 2)"}, "extract `code` (no space)");
    }
    {
        std::string_view j = R"json({"missing_key":"v"})json";
        CHECK_EQ(json_field(j, "cmd"), std::string{""}, "missing field returns empty");
    }
    {
        // Field value contains an escaped quote
        std::string_view j = R"json({"name": "a\"b"})json";
        CHECK_EQ(json_field(j, "name"), std::string{"a\"b"}, "unescape \\\" inside value");
    }
    {
        // Field value contains a smuggled NUL byte (control char) — field
        // extractor should round-trip the raw byte through the unescape path
        // since the wire format encoded it as \u0000.
        std::string_view j = R"json({"x": "a\u0000b"})json";
        std::string got = json_field(j, "x");
        std::string expected;
        expected.push_back('a');
        expected.push_back('\0');
        expected.push_back('b');
        std::println("  DEBUG: got.sz={} expected.sz={}", got.size(), expected.size());
        CHECK_EQ(got, expected, R"(\u0000 unescapes to NUL byte inside value)");
    }
    return g_failed == 0;
}

// ── AC4: §1 cap behavior simulated ───────────────────────────────────

static bool ac_cap_behavior() {
    std::println("\n--- AC6: cap math sanity ---");
    // kMaxServeAsyncLineBytes should be > 1 KiB (so we don't disrupt
    // legitimate small requests) and < 64 MiB (so a malicious client can't
    // bound more than 4 KiB/s × 64M = too much hand-waving, this is just
    // a sanity window).
    CHECK(kMaxServeAsyncLineBytes >= (1u << 10),
          "cap >= 1 KiB (don't reject legitimate large requests)");
    CHECK(kMaxServeAsyncLineBytes <= (64u << 20), "cap <= 64 MiB (bounded memory)");
    return g_failed == 0;
}

// ── AC5: §7/§6/§8 compile-time guards (header presence + symbols) ────

static bool ac_header_guards() {
    std::println("\n--- AC7: serve_async.h expose + Tier-1 surface ---");
    // We rely on inline-namespace detail being present so other code can
    // use the helpers (this is a structural guard — if the namespace
    // changes, this test fails to compile).
    auto sz = kMaxServeAsyncLineBytes;
    CHECK(sz > 0, "kMaxServeAsyncLineBytes visible at compile time");
    // Smoke-test: every input `"` becomes `\"` in the output (backslash
    // immediately before quote). We check by counting that there's at
    // least one `\"` token when the input contains a quote.
    std::string_view plain = R"json(value with "quote")json";
    std::string esc = json_escape(plain);
    CHECK(esc.find(R"(\")") != std::string::npos, "input `\"` becomes `\\\"` in output");
    CHECK(esc.find(R"(\n)") == std::string::npos, "no false \\n escape when no newline in input");
    // Same check for tab: literal tab becomes \t
    std::string esc2 = json_escape("a\tb");
    CHECK_EQ(esc2, std::string{R"(a\tb)"}, "tab becomes \\t");
    return g_failed == 0;
}

// ── AC8: §7 libcurl multi-soname fallback (runtime + static) ─────────

static bool ac_libcurl_fallback() {
    std::println("\n--- AC8: §7 libcurl dlopen multi-soname fallback ---");
    // Static check: the source files declare a kSonames array with the
    // platform-specific names. If anyone reverts to a single hardcoded
    // "libcurl.so.4" string, this test fails.
    auto io_src = slurp(std::string(AURA_TEST_SRC_DIR) + "/compiler/evaluator_primitives_io.cpp");
    auto serve_src = slurp(std::string(AURA_TEST_SRC_DIR) + "/serve/serve_async.cpp");
    CHECK(!io_src.empty(), "evaluator_primitives_io.cpp readable");
    CHECK(!serve_src.empty(), "serve_async.cpp readable");
    // Required platform sonames — the full list is bigger but these four
    // are the canonical ones. Add as needed if the list grows.
    CHECK(io_src.find("libcurl.so.4") != std::string::npos,
          "evaluator_primitives_io.cpp mentions libcurl.so.4");
    CHECK(io_src.find("libcurl.so") != std::string::npos,
          "evaluator_primitives_io.cpp mentions libcurl.so");
    CHECK(io_src.find("libcurl.4.dylib") != std::string::npos,
          "evaluator_primitives_io.cpp mentions macOS libcurl.4.dylib");
    CHECK(io_src.find("libcurl.dylib") != std::string::npos,
          "evaluator_primitives_io.cpp mentions macOS libcurl.dylib");
    // serve_async has its own duplicate kSonames (the §8 in-process HTTP
    // path duplicated the struct). Same set of names expected.
    CHECK(serve_src.find("libcurl.so.4") != std::string::npos,
          "serve_async.cpp mentions libcurl.so.4");
    // Runtime check: at least one of the candidate sonames must load on
    // this platform (Linux + libcurl installed). If libcurl isn't on the
    // host we can't dlopen any of them — skip with a notice rather than
    // failing, since the fallback is correct on systems that have libcurl.
    static constexpr const char* kCandidates[] = {
        "libcurl.so.4",
        "libcurl.so",
        "libcurl.4.dylib",
        "libcurl.dylib",
    };
    int loaded = 0;
    for (auto* name : kCandidates) {
        if (auto* h = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL)) {
            ++loaded;
            // Verify the loadable handle exposes the symbols we need.
            auto* ei = (void*)::dlsym(h, "curl_easy_init");
            auto* sa = (void*)::dlsym(h, "curl_slist_append");
            CHECK(ei && sa, std::string{"dlopen(\""} + name +
                                "\") exposes curl_easy_init + curl_slist_append");
            ::dlclose(h);
        }
    }
    if (loaded > 0) {
        std::println("  PASS: {} candidate soname(s) loaded on this platform", loaded);
        ++g_passed;
    } else {
        std::println("  SKIP: no libcurl candidate soname loads on this platform "
                     "(expected: libcurl isn't installed in this CI image)");
        // Not a failure — the fallback is correct, just unverifiable here.
        ++g_passed;
    }
    return g_failed == 0;
}

// ── AC9: §6 git-commit fork+execlp (replaces ::system) ──────────────

static bool ac_git_commit_exec() {
    std::println("\n--- AC9: §6 git-commit fork+execlp replaces ::system ---");
    auto io_src = slurp(std::string(AURA_TEST_SRC_DIR) + "/compiler/evaluator_primitives_io.cpp");
    CHECK(!io_src.empty(), "evaluator_primitives_io.cpp readable");
    // The fix replaces the legacy `::system("git commit -m ... 2>/dev/null")`
    // call with explicit fork+execlp. We assert both halves: the new
    // tokens are present AND the old `::system("git commit` token is absent.
    CHECK(io_src.find("::execlp(\"git\", \"git\", \"commit\"") != std::string::npos,
          "fork+execlp(\"git\", \"git\", \"commit\", ...) is present in git-commit path");
    CHECK(io_src.find("pid_t pid = ::fork()") != std::string::npos,
          "fork() is called before execlp (parent/child split)");
    CHECK(io_src.find("::waitpid(pid") != std::string::npos,
          "parent waitpid()s the child for exit-code propagation");
    // The fix redirects stderr to /dev/null in the child to preserve the
    // legacy silent-fail behavior of the old `2>/dev/null` redirection.
    CHECK(io_src.find("/dev/null") != std::string::npos,
          "child redirects stderr to /dev/null (preserves legacy silent-fail)");
    // Negative: the legacy `::system("git commit` pattern is gone.
    CHECK(io_src.find("::system(\"git commit") == std::string::npos,
          "legacy ::system(\"git commit ...\") shell path is gone");
    // Negative: the legacy single-quote escape loop is gone too.
    CHECK(io_src.find("esc += \"'\\\\''\"") == std::string::npos,
          "legacy single-quote escape loop is gone (no shell-quoting needed)");
    return g_failed == 0;
}

// ── AC10: §8 auth token never on cmdline (in-process libcurl) ──────

static bool ac_auth_token_in_process() {
    std::println("\n--- AC10: §8 auth header set via CURLOPT_HTTPHEADER, never argv ---");
    auto serve_src = slurp(std::string(AURA_TEST_SRC_DIR) + "/serve/serve_async.cpp");
    CHECK(!serve_src.empty(), "serve_async.cpp readable");
    // The fix introduces http_post_in_process() which sets the auth header
    // via curl's slist API. We assert the helper exists and uses the right
    // option constant.
    CHECK(serve_src.find("http_post_in_process") != std::string::npos,
          "http_post_in_process helper is defined");
    CHECK(serve_src.find("CURLOPT_HTTPHEADER") != std::string::npos,
          "in-process HTTP path uses CURLOPT_HTTPHEADER");
    CHECK(serve_src.find("curl_slist_append") != std::string::npos ||
              serve_src.find("slist_append(headers") != std::string::npos,
          "auth header is appended via curl_slist API (in-process only)");
    // Negative: the g_http_post_async lambda in run_serve_async must NOT
    // contain a fork+execvp curl with the auth header pushed into argv.
    // We look for the regression pattern: "Authorization: Bearer" appearing
    // inside an argv.push_back chain in serve_async's main g_http_post_async
    // handler (the bench path retains the old pattern and is explicitly
    // out of scope for #473 §8 — see MEMORY note).
    auto main_start = serve_src.find("aura::messaging::g_http_post_async");
    CHECK(main_start != std::string::npos, "main g_http_post_async lambda is registered");
    if (main_start != std::string::npos) {
        // Slice from the main handler to the next major registration
        // (session:create) to scope the negative check.
        auto slice_end = serve_src.find("g_session_create", main_start);
        std::string slice = (slice_end == std::string::npos)
                                ? serve_src.substr(main_start)
                                : serve_src.substr(main_start, slice_end - main_start);
        // The main handler should NOT contain argv.push_back( for auth.
        // The simple check: the substring "argv.push_back(\"-H\")" must
        // NOT appear, because that means the old fork+exec path is back.
        CHECK(slice.find("argv.push_back") == std::string::npos,
              "main g_http_post_async handler does not push argv tokens "
              "(no fork+exec curl in the security-sensitive path)");
        // The handler delegates to http_post_in_process via a worker thread.
        CHECK(slice.find("http_post_in_process") != std::string::npos,
              "main handler delegates to http_post_in_process helper");
    }
    return g_failed == 0;
}

int main() {
    std::println("=== Issue #473 Tier 1: serve-async security hardening ===");
    ac_cap_constant();
    ac_escape_basic();
    ac_escape_control();
    ac_escape_mixed();
    ac_field_extract();
    ac_cap_behavior();
    ac_header_guards();
    ac_libcurl_fallback();
    ac_git_commit_exec();
    ac_auth_token_in_process();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
