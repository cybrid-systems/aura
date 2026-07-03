// @category: integration
// @reason: tests Issue #676 sandbox capability + mutation audit primitives

// test_issue_676.cpp — Issue #676: Security model, sandboxing & audit.
//
// Scope-limited close ships:
//   - sandbox_mode + capability enforcement on io/mutate/exec
//   - security:set-sandbox-mode! / security:grant-capability!
//   - query:security-stats + query:mutation-audit-log
//   - audit events emitted on successful Guard mutations
//
// AC1: sandbox off — write-file succeeds without capability
// AC2: sandbox on — write-file denied without io-write
// AC3: sandbox on + io-write grant — write-file allowed
// AC4: sandbox on — mutate:rebind denied without mutate cap
// AC5: sandbox on + mutate grant — mutate succeeds + audit log grows
// AC6: query:security-stats reports denials
// AC7: stats:list includes query:security-stats + query:mutation-audit-log
// AC8: stats:count >= 53

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace fs = std::filesystem;
namespace aura_issue_676_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                                std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t sec_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:security-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r)) return -1;
    return aura::compiler::types::as_int(*r);
}

static bool is_error_val(const aura::compiler::types::EvalValue& v) {
    return aura::compiler::types::is_error(v);
}

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; std::println(std::cout, "  PASS: {}", msg); } \
    else { ++g_failed; std::println(std::cerr, "  FAIL: {}", msg); } \
} while (0)

}  // namespace aura_issue_676_detail

int main() {
    using namespace aura_issue_676_detail;

    std::println("=== Issue #676: sandbox + mutation audit ===");

    const auto tmp = fs::temp_directory_path() / "aura_issue_676_test.txt";
    std::ofstream(tmp) << "before";
    const auto path_lit = std::format("\"{}\"", tmp.string());

    aura::compiler::CompilerService cs;

    // AC1: sandbox off — write allowed
    {
        std::println("\n--- AC1: write-file without sandbox ---");
        run_on(cs, "(security:set-sandbox-mode! #f)");
        auto r = run_on(cs, std::format("(write-file {} \"ok\")", path_lit));
        CHECK(!is_error_val(r), "write-file succeeds when sandbox off");
    }

    // AC2: sandbox on — write denied
    {
        std::println("\n--- AC2: write-file denied in sandbox ---");
        run_on(cs, "(security:set-sandbox-mode! #t)");
        auto r = run_on(cs, std::format("(write-file {} \"nope\")", path_lit));
        CHECK(is_error_val(r), "write-file denied without io-write capability");
        CHECK(sec_int(cs, "capability-denials") >= 1, "denial counter bumped");
    }

    // AC3: grant io-write — write allowed
    {
        std::println("\n--- AC3: write-file with io-write grant ---");
        run_on(cs, "(security:grant-capability! \"io-write\")");
        auto r = run_on(cs, std::format("(write-file {} \"granted\")", path_lit));
        CHECK(!is_error_val(r), "write-file succeeds with io-write grant");
        std::ifstream in(tmp);
        std::string content;
        std::getline(in, content);
        CHECK(content == "granted", "file content updated after granted write");
    }

    // AC4/AC5: mutate sandbox + audit
    {
        std::println("\n--- AC4/AC5: mutate capability + audit log ---");
        run_on(cs, std::format("(set-code \"(define x 1)\")"));
        run_on(cs, "(eval-current)");
        const auto before = sec_int(cs, "mutation-audit-total");
        const auto denials_before = sec_int(cs, "capability-denials");
        auto denied = run_on(cs, "(mutate:rebind \"x\" \"2\")");
        const auto denials_after = sec_int(cs, "capability-denials");
        CHECK(denials_after > denials_before,
              "mutate:rebind denied without mutate capability (denial counter)");
        CHECK(!aura::compiler::types::is_int(denied),
              "mutate:rebind does not return success int when denied");
        run_on(cs, "(security:grant-capability! \"mutate\")");
        run_on(cs, "(mutate:rebind \"x\" \"99\")");
        run_on(cs, "(eval-current)");
        auto val = run_on(cs, "x");
        CHECK(aura::compiler::types::is_int(val) && aura::compiler::types::as_int(val) == 99,
              "mutate:rebind succeeds with mutate grant (x == 99)");
        const auto after = sec_int(cs, "mutation-audit-total");
        CHECK(after > before, std::format("mutation-audit-total grew ({} -> {})", before, after));
        auto log = run_on(cs, "(query:mutation-audit-log 5)");
        CHECK(aura::compiler::types::is_pair(log) || aura::compiler::types::is_void(log),
              "query:mutation-audit-log returns list");
    }

    // AC7: stats:list
    {
        std::println("\n--- AC7: stats:list includes security primitives ---");
        auto r = run_on(cs,
            "(letrec ((find? (lambda (needle hay) "
            "                (if (pair? hay) "
            "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
            "                    #f)))) "
            "  (and (find? \"query:security-stats\" (stats:list)) "
            "       (find? \"query:mutation-audit-log\" (stats:list))))");
        CHECK(aura::compiler::types::is_bool(r) && aura::compiler::types::as_bool(r),
              "stats:list includes query:security-stats and query:mutation-audit-log");
    }

    // AC8: stats:count
    {
        std::println("\n--- AC8: stats:count >= 53 ---");
        auto r = run_on(cs, "(stats:count)");
        const auto n = aura::compiler::types::is_int(r) ? aura::compiler::types::as_int(r) : 0;
        CHECK(n >= 53, std::format("stats:count >= 53 (got {})", n));
    }

    fs::remove(tmp);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}