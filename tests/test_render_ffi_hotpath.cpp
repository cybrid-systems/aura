// test_render_ffi_hotpath.cpp — Issue #1354: c-* render hot path + c-render-bind

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // Baseline query
    {
        auto h = cs.eval("(query:render-ffi-available)");
        CHECK(h && is_hash(*h), "query:render-ffi-available is hash");
        CHECK(href(cs, "query:render-ffi-available", "schema") == 1354, "schema 1354");
        CHECK(href(cs, "query:render-ffi-available", "active") == 1, "active");
        CHECK(href(cs, "query:render-ffi-available", "phase") == 2, "phase 2");
    }

    const auto enter0 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
    const auto reg0 = href(cs, "query:render-ffi-available", "registered");

    // c-alloc / c-free bump hotpath enter (wrap all c-*)
    {
        auto o = cs.eval("(c-alloc 64)");
        CHECK(o.has_value(), "c-alloc");
        (void)cs.eval("(c-free (c-alloc 32))");
        auto enter1 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        CHECK(enter1 > enter0, "c-alloc/c-free enter render hotpath");
    }

    // c-struct-size also hotpathed
    {
        const auto e0 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        auto s = cs.eval("(c-struct-size 8 8)");
        CHECK(s && is_int(*s) && as_int(*s) == 16, "c-struct-size");
        auto e1 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        CHECK(e1 > e0, "c-struct-size hotpath enter");
    }

    // c-render-bind with RTLD_DEFAULT — use a well-known libc symbol if present
    // (strlen). Signature is metadata for Agent discovery.
    {
        auto b = cs.eval(R"((c-render-bind -1 "ansi-emit" "strlen" "(String) -> Int"))");
        CHECK(b && is_bool(*b) && as_bool(*b), "c-render-bind strlen as ansi-emit");
        auto reg1 = href(cs, "query:render-ffi-available", "registered");
        CHECK(reg1 == reg0 + 1, "registered == 1 after bind");
        auto ok = href(cs, "query:render-ffi-available", "bind-success");
        CHECK(ok >= 1, "bind-success >= 1");
    }

    // Second binding grows registry
    {
        auto b = cs.eval(R"((c-render-bind -1 "c-present-batch" "strlen" "(String) -> Int"))");
        CHECK(b && is_bool(*b) && as_bool(*b), "c-render-bind second");
        auto reg2 = href(cs, "query:render-ffi-available", "registered");
        CHECK(reg2 == reg0 + 2, "registered grows");
    }

    // c-render-call records hot_path_dispatches
    {
        const auto d0 = href(cs, "query:render-ffi-available", "hot-path-dispatches");
        auto c = cs.eval(R"((c-render-call "ansi-emit"))");
        CHECK(c && is_bool(*c) && as_bool(*c), "c-render-call registered binding");
        auto d1 = href(cs, "query:render-ffi-available", "hot-path-dispatches");
        CHECK(d1 == d0 + 1, "hot-path-dispatches increments");
        auto calls = href(cs, "query:render-ffi-available", "binding-calls");
        CHECK(calls >= 1, "binding-calls >= 1");
    }

    // Unknown binding call fails
    {
        auto c = cs.eval(R"((c-render-call "no-such-binding"))");
        CHECK(c && is_bool(*c) && !as_bool(*c), "unknown binding → #f");
    }

    // query:render-ffi-count matches registered
    {
        auto n = cs.eval("(query:render-ffi-count)");
        CHECK(n && is_int(*n) && as_int(*n) == href(cs, "query:render-ffi-available", "registered"),
              "query:render-ffi-count matches");
    }

    // Invalid bind args
    {
        auto b = cs.eval("(c-render-bind)");
        CHECK(b && is_bool(*b) && !as_bool(*b), "c-render-bind arity fail");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("render FFI hotpath #1354: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
