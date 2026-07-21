// @category: integration
// @reason: Issue #1911 B8 — leap-year? / days-in-month correct on direct call.
// Root causes: (1) IR `and` short-circuit emitted ConstVoid (truthy);
// (2) type-checker register_primitive shadow for pure stdlib calendar helpers.
//
//   AC1: type_checker drops leap-year?/days-in-month register_primitive
//   AC2: lowering_impl and-nope uses Local val (not ConstVoid); cites #1911
//   AC3: (require std/datetime) + direct (days-in-month 2023 2) → 28
//   AC4: (days-in-month 1900 2) → 28; (days-in-month 2024 2) → 29
//   AC5: (leap-year? 2023) → #f; (leap-year? 1900) → #f; (leap-year? 2024) → #t
//   AC6: direct matches let-bound

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool require_datetime(CompilerService& cs) {
    // Prefer all: import so exports bind in the current env.
    auto r = cs.eval("(require \"std/datetime\" all:)");
    if (r.has_value())
        return true;
    r = cs.eval("(require \"std/datetime\")");
    return r.has_value();
}

static std::int64_t eval_int(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_int(*r))
        return -99999;
    return as_int(*r);
}

static int eval_bool01(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_bool(*r))
        return -1;
    return as_bool(*r) ? 1 : 0;
}

} // namespace

int main() {
    // ── AC1 / AC2: source ──
    {
        std::println("\n--- AC1/AC2: drop register_primitive for calendar helpers ---");
        std::string src;
        for (const char* p :
             {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read type_checker_impl.cpp");
        CHECK(src.find("#1911") != std::string::npos, "cites #1911");
        CHECK(src.find("B8") != std::string::npos, "cites B8");
        CHECK(src.find("#1906") != std::string::npos, "cites #1906 lineage");
        auto pos_leap = src.find("register_primitive(\"leap-year?\"");
        auto pos_dim = src.find("register_primitive(\"days-in-month\"");
        CHECK(pos_leap == std::string::npos, "no leap-year? register_primitive");
        CHECK(pos_dim == std::string::npos, "no days-in-month register_primitive");
        std::string low;
        for (const char* p :
             {"src/compiler/lowering_impl.cpp", "../src/compiler/lowering_impl.cpp"}) {
            low = read_file(p);
            if (!low.empty())
                break;
        }
        CHECK(!low.empty(), "read lowering_impl.cpp");
        CHECK(low.find("#1911") != std::string::npos, "lowering cites #1911");
        // and short-circuit nope arm must use Local (falsy val), not ConstVoid.
        auto and_pos = low.find("callee_name == \"and\"");
        CHECK(and_pos != std::string::npos, "and lowering present");
        auto and_win = low.substr(and_pos, 3500);
        CHECK(and_win.find("result_slot, val") != std::string::npos, "and nope uses Local val");
        // Nope arm must not reintroduce ConstVoid for short-circuit result.
        auto nope = and_win.find("nope block");
        CHECK(nope != std::string::npos, "nope block comment");
        auto nope_win = and_win.substr(nope, 200);
        CHECK(nope_win.find("ConstVoid") == std::string::npos, "and nope does not ConstVoid");
        std::string aura;
        for (const char* p : {"lib/std/datetime.aura", "../lib/std/datetime.aura"}) {
            aura = read_file(p);
            if (!aura.empty())
                break;
        }
        CHECK(!aura.empty(), "read datetime.aura");
        CHECK(aura.find("(define (leap-year?") != std::string::npos, "stdlib leap-year?");
        CHECK(aura.find("(define (days-in-month") != std::string::npos, "stdlib days-in-month");
    }

    // ── AC3–AC6: runtime direct application ──
    {
        std::println("\n--- AC3–AC6: direct stdlib calendar (no shadow) ---");
        CompilerService cs;
        CHECK(require_datetime(cs), "require std/datetime");

        // AC3: non-leap Feb 2023 → 28 (bug was 29 / void truthy from IR and)
        CHECK(eval_int(cs, "(days-in-month 2023 2)") == 28, "2023-02 → 28");
        // AC4
        CHECK(eval_int(cs, "(days-in-month 1900 2)") == 28, "1900-02 → 28 (not century leap)");
        CHECK(eval_int(cs, "(days-in-month 2024 2)") == 29, "2024-02 → 29 leap");
        CHECK(eval_int(cs, "(days-in-month 2023 4)") == 30, "2023-04 → 30");
        CHECK(eval_int(cs, "(days-in-month 2023 1)") == 31, "2023-01 → 31");

        // AC5: leap-year?
        CHECK(eval_bool01(cs, "(leap-year? 2023)") == 0, "2023 not leap");
        CHECK(eval_bool01(cs, "(leap-year? 1900)") == 0, "1900 not leap");
        CHECK(eval_bool01(cs, "(leap-year? 2024)") == 1, "2024 leap");
        CHECK(eval_bool01(cs, "(leap-year? 2000)") == 1, "2000 leap (400 rule)");

        // AC6: direct == let-bound (pre-fix workaround path)
        CHECK(eval_int(cs, "(let ((dim days-in-month)) (dim 2023 2))") == 28, "let dim 2023");
        CHECK(eval_int(cs, "(days-in-month 2023 2)") ==
                  eval_int(cs, "(let ((dim days-in-month)) (dim 2023 2))"),
              "direct == let-bound 2023");
        CHECK(eval_bool01(cs, "(leap-year? 2023)") ==
                  eval_bool01(cs, "(let ((ly leap-year?)) (ly 2023))"),
              "direct == let-bound leap-year?");
    }

    std::println("\n=== test_datetime_shadow_1911: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
