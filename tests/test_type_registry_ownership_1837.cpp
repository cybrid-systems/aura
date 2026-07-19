// @category: unit
// @reason: Issue #1837 — type_registry_ is a documented non-owning
// (or single-owner via ensure_type_registry) raw pointer. Production
// CompilerService wires &type_registry_ once; concurrent rebind/free
// while compile:hw-coercion-* runs is unsupported (#1835 sibling).
//
//   AC1: evaluator.ixx / ctor cite #1837 ownership contract
//   AC2: set_type_registry / ensure under quiescence works
//   AC3: CompilerService wires type_registry_ (service lifetime)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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
        std::println("\n--- AC1: ownership contract documented ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1837") != std::string::npos, "ixx cites #1837");
        CHECK(ixx.find("set_type_registry") != std::string::npos, "setter present");
        CHECK(ixx.find("non-owning") != std::string::npos ||
                  ixx.find("CompilerService::type_registry_") != std::string::npos,
              "documents service ownership");

        std::string ctor;
        for (const char* p :
             {"src/compiler/evaluator_ctor.cpp", "../src/compiler/evaluator_ctor.cpp"}) {
            ctor = read_file(p);
            if (!ctor.empty())
                break;
        }
        CHECK(!ctor.empty(), "read evaluator_ctor.cpp");
        CHECK(ctor.find("#1837") != std::string::npos, "ctor cites #1837");
    }

    // ── AC2: set/ensure under quiescence ──
    {
        std::println("\n--- AC2: set_type_registry / ensure under quiescence ---");
        Evaluator ev;
        CHECK(ev.type_registry_ptr() == nullptr, "starts null");
        auto* owned = ev.ensure_type_registry();
        CHECK(owned != nullptr, "ensure creates");
        CHECK(ev.type_registry_ptr() == owned, "ptr matches");
        // Clear via external null (owned registry is deleted only when
        // replacing a previously owned one — null adopts external).
        ev.set_type_registry(nullptr);
        CHECK(ev.type_registry_ptr() == nullptr, "cleared");
    }

    // ── AC3: service wires registry for service lifetime ──
    {
        std::println("\n--- AC3: CompilerService wires type_registry_ ---");
        CompilerService cs;
        // Production path: set_type_registry(&type_registry_) in ctor —
        // same non-owning pattern as #1835 metrics_ (not concurrent rebind).
        CHECK(cs.evaluator().type_registry_ptr() != nullptr, "service wires registry");
        // Still non-null after a normal eval (no rebind).
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok");
        CHECK(cs.evaluator().type_registry_ptr() != nullptr, "registry stable after eval");
    }

    std::println("\n=== test_type_registry_ownership_1837: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
