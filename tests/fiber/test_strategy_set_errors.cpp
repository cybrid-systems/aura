// @category: unit
// @reason: Issue #1714 — strategy:set-strategy must return tagged merr
// Issue #1714 (#1978 renamed): issue# moved from filename to header.
// on bad args / unknown name (not silent make_void).
//
//   AC1: unknown strategy name → pair merr (car "unknown-strategy")
//   AC2: missing/non-string arg → pair merr (car "bad-arg")
//   AC3: valid strategy still returns int; active unchanged by bad set
//   AC4: source uses make_merr (not make_void) on invalid paths

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// make_merr is (kind . (msg . 0)); car of result is kind string.
bool car_is_string(CompilerService& cs, std::string_view expr, std::string_view expect) {
    auto r = cs.eval(std::string("(car ") + std::string(expr) + ")");
    if (!r || !is_string(*r))
        return false;
    auto& heap = cs.evaluator().string_heap_mut();
    auto idx = as_string_idx(*r);
    if (idx >= heap.size())
        return false;
    return heap[idx] == expect;
}

} // namespace

int main() {
    // ── AC1: unknown name ──
    {
        std::println("\n--- AC1: unknown strategy → unknown-strategy merr ---");
        CompilerService cs;
        auto r = cs.eval("(strategy:set-strategy \"not-a-real-strategy\")");
        CHECK(r.has_value(), "eval ok");
        CHECK(r && is_pair(*r), "returns pair merr");
        CHECK(!is_void(*r), "not silent void");
        CHECK(car_is_string(cs, "(strategy:set-strategy \"typo-name\")", "unknown-strategy"),
              "car tag unknown-strategy");
    }

    // ── AC2: bad args ──
    {
        std::println("\n--- AC2: bad-arg paths ---");
        CompilerService cs;
        auto empty = cs.eval("(strategy:set-strategy)");
        CHECK(empty.has_value() && is_pair(*empty), "no-arg → merr pair");
        CHECK(car_is_string(cs, "(strategy:set-strategy)", "bad-arg"), "empty → bad-arg");

        auto wrong = cs.eval("(strategy:set-strategy 42)");
        CHECK(wrong.has_value() && is_pair(*wrong), "int arg → merr pair");
        CHECK(car_is_string(cs, "(strategy:set-strategy 99)", "bad-arg"), "non-string → bad-arg");
    }

    // ── AC3: valid still works; bad does not clobber active ──
    {
        std::println("\n--- AC3: valid set + bad does not clobber ---");
        CompilerService cs;
        auto ok = cs.eval("(strategy:set-strategy \"coverage-greedy\")");
        CHECK(ok.has_value() && is_int(*ok), "valid returns int");
        if (ok && is_int(*ok))
            CHECK(as_int(*ok) > 0, "int is strategy name length");

        (void)cs.eval("(strategy:set-strategy \"nope\")");
        auto active = cs.eval("(strategy:active)");
        CHECK(active.has_value() && is_string(*active), "active still set");
        if (active && is_string(*active)) {
            auto& heap = cs.evaluator().string_heap_mut();
            auto idx = as_string_idx(*active);
            CHECK(idx < heap.size() && heap[idx] == "coverage-greedy",
                  "active remains coverage-greedy after bad set");
        }
    }

    // ── AC4: source audit ──
    {
        std::println("\n--- AC4: source make_merr not make_void ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1714") != std::string::npos, "cites #1714");
            auto pos = src.find("add(\"strategy:set-strategy\"");
            CHECK(pos != std::string::npos, "found set-strategy");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 2000 : end - pos);
                CHECK(win.find("make_merr") != std::string::npos, "uses make_merr");
                CHECK(win.find("unknown-strategy") != std::string::npos, "unknown-strategy tag");
                // No make_void in the set-strategy body (error paths fixed).
                CHECK(win.find("make_void") == std::string::npos, "no make_void in body");
            }
        }
    }

    std::println("\n=== test_strategy_set_errors_1714: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
