// @category: unit
// @reason: Issue #4936 B4 — registration calls a function_ref and does not keep it.
//
//   AC1: an owning std::function outlives the function_ref parameter and receives the name
//   AC2: a temporary std::function passed into register_prim is invoked before it dies
//   AC3: the callback's string is a copy, so clearing the caller's buffer does not change it

#include "compiler/prim_registrar_scaffold.hh"

#include <functional>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

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

struct DummyEvaluator {};

void call_once(std::function_ref<void(std::string, int)> add) {
    add(std::string("kept"), 1);
}

} // namespace

int main() {
    int calls = 0;
    std::string got;
    std::function<void(std::string, int)> owner = [&](std::string name, int fn) {
        got = std::move(name);
        calls += fn;
    };
    std::function_ref<void(std::string, int)> ref = owner;
    DummyEvaluator ev;
    aura::compiler::register_prim(ref, ev, std::string_view("plus"), 3);
    expect(calls == 3 && got == "plus", "owning function receives the copied name");
    owner = [&](std::string name, int fn) {
        got = std::move(name);
        calls += fn;
    };
    aura::compiler::register_prim(
        std::function<void(std::string, int)>([&](std::string name, int fn) {
            got = std::move(name);
            calls += fn;
        }),
        ev, std::string_view("minus"), 4);
    expect(calls == 7 && got == "minus", "temporary registrar is invoked before it dies");

    std::string scratch = "kept";
    call_once([&](std::string name, int) { got = std::move(name); });
    scratch.clear();
    expect(got == "kept", "callback owns its string after the caller buffer is cleared");
    expect(calls == 7, "the lifetime check did not invent another registration");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
