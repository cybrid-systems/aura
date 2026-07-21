// @category: unit
// @reason: Issue #1850 — compile:hw-bitvec-register must wrap
// Issue #1837/#1850/#1897 (#1978 renamed): issue# moved from filename to header.
// TypeRegistry mutations (register_type / register_hw_bitvec) in
// MutationBoundaryGuard + try/catch; type_registry_ raw pointee
// follows #1837 ownership/quiescence (not shared_ptr).
//
//   AC1: source cites #1850; Guard + try/catch; #1837 ownership note
//   AC2: register returns 1; width/signed? readable after
//   AC3: nested under outer Guard still completes

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
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: Guard + ownership on hw-bitvec-register ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_06.cpp");
        CHECK(src.find("#1850") != std::string::npos, "cites #1850");
        auto pos = src.find("\"compile:hw-bitvec-register\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 2200);
        // #1897: may use shared run_under_mutation_guard (try_acquire + try/catch).
        const bool via_helper = win.find("run_under_mutation_guard") != std::string::npos;
        const bool via_guard = win.find("MutationBoundaryGuard") != std::string::npos &&
                               win.find("guard_ok") != std::string::npos;
        CHECK(via_helper || via_guard, "uses Guard or try_acquire helper");
        CHECK(win.find("register_hw_bitvec") != std::string::npos, "calls register_hw_bitvec");
        if (!via_helper) {
            CHECK(win.find("try {") != std::string::npos || win.find("try{") != std::string::npos,
                  "try block");
            CHECK(win.find("catch") != std::string::npos, "catch path");
        }
        // Ownership note sits above the add() site (#1837).
        auto pre = src.substr(pos > 600 ? pos - 600 : 0, 600);
        CHECK(pre.find("#1837") != std::string::npos || win.find("#1837") != std::string::npos,
              "documents type_registry ownership (#1837)");
        CHECK(pre.find("quiescence") != std::string::npos ||
                  win.find("quiescence") != std::string::npos ||
                  pre.find("non-owning") != std::string::npos ||
                  win.find("non-owning") != std::string::npos,
              "documents non-owning / quiescence");
    }

    // ── AC2: runtime register + query ──
    {
        std::println("\n--- AC2: hw-bitvec-register then width/signed? ---");
        CompilerService cs;
        auto r = cs.eval("(compile:hw-bitvec-register \"uint16_t\" 16 0)");
        CHECK(r && is_int(*r) && as_int(*r) == 1, "register returns 1");
        auto w = cs.eval("(compile:hw-bitvec-width \"uint16_t\")");
        CHECK(w && is_int(*w) && as_int(*w) == 16, "width 16");
        auto s = cs.eval("(compile:hw-bitvec-signed? \"uint16_t\")");
        CHECK(s && is_int(*s) && as_int(*s) == 0, "unsigned");
        // Auto-create type name + signed.
        auto r2 = cs.eval("(compile:hw-bitvec-register \"auto_i8\" 8 1)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 1, "auto-create register 1");
        auto s2 = cs.eval("(compile:hw-bitvec-signed? \"auto_i8\")");
        CHECK(s2 && is_int(*s2) && as_int(*s2) == 1, "signed");
        // Idempotent re-register.
        auto r3 = cs.eval("(compile:hw-bitvec-register \"uint16_t\" 16 0)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 1, "idempotent re-register");
    }

    // ── AC3: nested Guard ──
    {
        std::println("\n--- AC3: under outer MutationBoundaryGuard ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        bool ok = true;
        {
            Evaluator::MutationBoundaryGuard outer(ev, &ok);
            CHECK(outer.is_outermost(), "outer is outermost");
            auto r = cs.eval("(compile:hw-bitvec-register \"nested_u32\" 32 0)");
            CHECK(r && is_int(*r) && as_int(*r) == 1, "register under outer Guard");
            CHECK(ev.mutation_boundary_depth_slot_value() >= 1, "depth held by outer");
        }
        CHECK(ok, "outer guard_ok");
        CHECK(ev.mutation_boundary_depth_slot_value() == 0, "depth 0 after outer dtor");
        auto w = cs.eval("(compile:hw-bitvec-width \"nested_u32\")");
        CHECK(w && is_int(*w) && as_int(*w) == 32, "width after outer Guard");
    }

    std::println("\n=== test_hw_bitvec_register_guard_1850: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
