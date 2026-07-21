// @category: integration
// @reason: Issue #1910 B1 — timestamp->date-string weekday table must match
// Sun=0..Sat=6 (same as weekday-name / timestamp->rfc822).
//
//   AC1: datetime.aura uses weekday-name (no Thu-first local table)
//   AC2: (timestamp->date-string 0) → "Thu 1 1970" (epoch Thursday)
//   AC3: (timestamp->weekday 0) → 4
//   AC4: date-string weekday prefix matches rfc822 weekday for epoch
//   AC5: Sun/Mon/Fri samples across first week of 1970

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

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(std::string(expr));
    if (!r || !is_bool(*r))
        return false;
    return as_bool(*r);
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

} // namespace

int main() {
    // ── AC1: source no longer uses Thu-first local table ──
    {
        std::println("\n--- AC1: source uses weekday-name (Sun=0..Sat=6) ---");
        std::string aura;
        for (const char* p : {"lib/std/datetime.aura", "../lib/std/datetime.aura"}) {
            aura = read_file(p);
            if (!aura.empty())
                break;
        }
        CHECK(!aura.empty(), "read datetime.aura");
        CHECK(aura.find("#1910") != std::string::npos, "cites #1910");
        CHECK(aura.find("B1") != std::string::npos, "cites B1");
        // Broken table must not reappear.
        CHECK(aura.find("\"Thu\" \"Fri\" \"Sat\" \"Sun\" \"Mon\" \"Tue\" \"Wed\"") ==
                  std::string::npos,
              "no Thu-first weekday table");
        auto pos = aura.find("(define (timestamp->date-string");
        CHECK(pos != std::string::npos, "define timestamp->date-string");
        auto win = aura.substr(pos, 280);
        CHECK(win.find("weekday-name") != std::string::npos, "date-string uses weekday-name");
    }

    // ── AC2–AC5: runtime ──
    {
        std::println("\n--- AC2–AC5: epoch and first-week weekday names ---");
        CompilerService cs;
        CHECK(require_datetime(cs), "require std/datetime");

        // AC3: Jan 1 1970 = Thursday = 4 (Sun=0..Sat=6)
        CHECK(eval_int(cs, "(timestamp->weekday 0)") == 4, "weekday epoch → 4");

        // AC2: previously emitted "Mon 1 1970" (wrong); must be Thu
        auto ds0 = eval_string(cs, "(timestamp->date-string 0)");
        CHECK(ds0 == "Thu 1 1970", "date-string epoch → Thu 1 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string 0) \"Thu 1 1970\")"),
              "string=? date-string epoch");

        // AC4: rfc822 still correct and shares weekday prefix
        auto rfc = eval_string(cs, "(timestamp->rfc822 0)");
        CHECK(rfc.starts_with("Thu,"), "rfc822 epoch starts Thu,");
        CHECK(eval_bool(cs, "(string=? (substring (timestamp->date-string 0) 0 3) "
                            "(substring (timestamp->rfc822 0) 0 3))"),
              "date-string weekday == rfc822 weekday");

        // AC5: first week of 1970
        // day0=Thu, day1=Fri, day2=Sat, day3=Sun, day4=Mon
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string 86400) \"Fri 2 1970\")"),
              "day1 Fri 2 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 3 86400)) \"Sun 4 1970\")"),
              "day3 Sun 4 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 4 86400)) \"Mon 5 1970\")"),
              "day4 Mon 5 1970");
        CHECK(eval_bool(cs, "(string=? (timestamp->date-string (* 6 86400)) \"Wed 7 1970\")"),
              "day6 Wed 7 1970");
    }

    std::println("\n=== test_datetime_date_string_1910: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
