// @category: unit
// @reason: Issue #5141 — add_hidden is the Batch 0 seam for #5138: the verb
//          reaches the body, the public surface does not.
//
//   AC1: lookup() finds a hidden body, so (query :op) / (mutate :op) still work
//   AC2: the hidden name gets no slot and no documented meta, so a direct
//        (mw:op) call stays unbound and api-reference cannot list it
//   AC3: a public add() next to it still takes a slot and stays enumerable

#include "core/sandbox.hh"

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

} // namespace

int main() {
    aura::compiler::CompilerService cs;
    auto& prims = cs.evaluator().primitives();

    const auto slots_before = prims.slot_count();
    const auto documented_before = prims.documented_meta_count();

    prims.add_hidden("__test_hidden_5141", [](std::span<const aura::compiler::types::EvalValue>) {
        return aura::compiler::types::make_int(7);
    });
    expect(prims.slot_count() == slots_before, "hidden registration adds no slot");
    expect(prims.documented_meta_count() == documented_before,
           "hidden registration adds no documented meta");
    expect(prims.slot_for_name("__test_hidden_5141") >= prims.slot_count(),
           "hidden name has no slot (a direct call stays unbound)");

    auto fn = prims.lookup("__test_hidden_5141");
    expect(fn.has_value(), "hidden body is reachable through lookup (verb dispatch)");
    if (fn) {
        const auto v = (*fn)({});
        expect(aura::compiler::types::is_int(v) && aura::compiler::types::as_int(v) == 7,
               "hidden body runs through lookup");
    }

    prims.add("__test_public_5141", [](std::span<const aura::compiler::types::EvalValue>) {
        return aura::compiler::types::make_int(9);
    });
    expect(prims.slot_count() == slots_before + 1, "public registration still takes a slot");
    expect(prims.slot_for_name("__test_public_5141") < prims.slot_count(),
           "public name is addressable by slot");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
