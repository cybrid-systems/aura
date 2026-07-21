// test_datetime.cpp — Merged datetime stdlib tests (#1978).
//
// Originally test_datetime_date_string_1910.cpp + test_datetime_shadow_1911.cpp.
// Both exercised std/datetime stdlib surface (weekday table + calendar
// helpers); merged to consolidate the datetime test surface.
//
// AC list:
//   Issue #1910 B1 — timestamp->date-string weekday table (Sun=0..Sat=6)
//     AC1: datetime.aura uses weekday-name (no Thu-first local table)
//     AC2: (timestamp->date-string 0) → "Thu 1 1970" (epoch Thursday)
//     AC3: (timestamp->weekday 0) → 4
//     AC4: date-string weekday prefix matches rfc822 weekday for epoch
//     AC5: Sun/Mon/Fri samples across first week of 1970
//   Issue #1911 B8 — leap-year? / days-in-month correct on direct call
//     AC1: type_checker drops leap-year?/days-in-month register_primitive
//     AC2: lowering_impl and-nope uses Local val (not ConstVoid)
//     AC3: (require std/datetime) + direct (days-in-month 2023 2) → 28
//     AC4: (days-in-month 1900 2) → 28; (days-in-month 2024 2) → 29
//     AC5: (leap-year? 2023) → #f; (leap-year? 1900) → #f; (leap-year? 2024) → #t
//     AC6: direct matches let-bound

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
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool require_datetime(CompilerService& cs) {
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

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    return eval_bool01(cs, expr) == 1;
}

static std::string eval_string(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_string(*r))
        return {};
    auto heap = cs.evaluator().string_heap();
    auto idx = as_string_idx(*r);
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

// ── AC1/AC2 (Issue #1911): source-level checks ──
static void check_source_1911() {
    std::println("\n--- AC1/AC2 (#1911): drop register_primitive for calendar helpers ---");
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
    for (const char* p : {"src/compiler/lowering_impl.cpp", "../src/compiler/lowering_impl.cpp"}) {
        low = read_file(p);
        if (!low.empty())
            break;
    }
    CHECK(!low.empty(), "read lowering_impl.cpp");
    CHECK(low.find("#1911") != std::string::npos, "lowering cites #1911");
    auto and_pos = low.find("callee_name == \"and\"");
    CHECK(and_pos != std::string::npos, "and lowering present");
    auto and_win = low.substr(and_pos, 3500);
    CHECK(and_win.find("result_slot, val") != std::string::npos, "and nope uses Local val");
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

// ── AC1 (Issue #1910): source-level weekday table check ──
static void check_source_1910() {
    std::println("\n--- AC1 (#1910): source uses weekday-name (Sun=0..Sat=6) ---");
    std::string aura;
    for (const char* p : {"lib/std/datetime.aura", "../lib/std/datetime.aura"}) {
        aura = read_file(p);
        if (!aura.empty())
            break;
    }
    CHECK(!aura.empty(), "read datetime.aura");
    CHECK(aura.find("#1910") != std::string::npos, "cites #1910");
    CHECK(aura.find("B1") != std::string::npos, "cites B1");
    CHECK(aura.find("\"Thu\" \"Fri\" \"Sat\" \"Sun\" \"Mon\" \"Tue\" \"Wed\"") == std::string::npos,
          "no Thu-first weekday table");
    auto pos = aura.find("(define (timestamp->date-string");
    CHECK(pos != std::string::npos, "define timestamp->date-string");
    auto win = aura.substr(pos, 280);
    CHECK(win.find("weekday-name") != std::string::npos, "date-string uses weekday-name");
}

} // namespace

int main() {
    // ── Source-level checks first (no CompilerService needed) ──
    check_source_1911();
    check_source_1910();

    // ── AC2–AC5 (#1910) and AC3–AC6 (#1911): runtime ──
    {
        std::println("\n--- AC2–AC5 (#1910) + AC3–AC6 (#1911): epoch + calendar ---");
        CompilerService cs;
        CHECK(require_datetime(cs), "require std/datetime");

        // #1910 AC3: Jan 1 1970 = Thursday = 4 (Sun=0..Sat=6)
        CHECK(eval_int(cs, "(timestamp->weekday 0)") == 4, "weekday epoch → 4");

        // #1910 AC2: previously emitted "Mon 1 1970" (wrong); must be Thu
        auto ds0 = eval_string(cs, "(timestamp->date-string 0)");
        CHECK(ds0 == "Thu 1 1970", "date-string epoch → Thu 1 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string 0) \"Thu 1 1970\")"),
              "string=? date-string epoch");

        // #1910 AC4: rfc822 weekday prefix matches
        auto rfc = eval_string(cs, "(timestamp->rfc822 0)");
        CHECK(rfc.starts_with("Thu,"), "rfc822 epoch starts Thu,");
        CHECK(eval_bool(cs, "(string=? (substring (timestamp->date-string 0) 0 3) "
                            "(substring (timestamp->rfc822 0) 0 3))"),
              "date-string weekday == rfc822 weekday");

        // #1910 AC5: first week of 1970 (day0=Thu, day1=Fri, day3=Sun, day4=Mon, day6=Wed)
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string 86400) \"Fri 2 1970\")"),
              "day1 Fri 2 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 3 86400)) \"Sun 4 1970\")"),
              "day3 Sun 4 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 4 86400)) \"Mon 5 1970\")"),
              "day4 Mon 5 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 6 86400)) \"Wed 7 1970\")"),
              "day6 Wed 7 1970");

        // #1911 AC3: non-leap Feb 2023 → 28 (bug was 29 / void truthy from IR and)
        CHECK(eval_int(cs, "(days-in-month 2023 2)") == 28, "2023-02 → 28");
        // #1911 AC4
        CHECK(eval_int(cs, "(days-in-month 1900 2)") == 28, "1900-02 → 28 (not century leap)");
        CHECK(eval_int(cs, "(days-in-month 2024 2)") == 29, "2024-02 → 29 leap");
        CHECK(eval_int(cs, "(days-in-month 2023 4)") == 30, "2023-04 → 30");
        CHECK(eval_int(cs, "(days-in-month 2023 1)") == 31, "2023-01 → 31");

        // #1911 AC5: leap-year?
        CHECK(!eval_bool(cs, "(leap-year? 2023)"), "2023 not leap");
        CHECK(!eval_bool(cs, "(leap-year? 1900)"), "1900 not leap");
        CHECK(eval_bool(cs, "(leap-year? 2024)"), "2024 leap");
        CHECK(eval_bool(cs, "(leap-year? 2000)"), "2000 leap (400 rule)");

        // #1911 AC6: direct == let-bound
        CHECK(eval_int(cs, "(let ((dim days-in-month)) (dim 2023 2))") == 28, "let dim 2023");
        CHECK(eval_int(cs, "(days-in-month 2023 2)") ==
                  eval_int(cs, "(let ((dim days-in-month)) (dim 2023 2))"),
              "direct == let-bound 2023");
        CHECK(eval_bool01(cs, "(leap-year? 2023)") ==
                  eval_bool01(cs, "(let ((ly leap-year?)) (ly 2023))"),
              "direct == let-bound leap-year?");
    }

    std::println("\n=== test_datetime (#1910 + #1911): {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
