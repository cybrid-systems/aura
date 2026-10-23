// @category: unit
// @reason: Issue #4631 — the hash CONSTRUCTOR and hash-set! silently DROPPED
//          the empty string as a key. The #2652 corruption guard treated
//          "" as a sentinel (make_string(0)) and skipped the pair / no-op'd
//          the write, so (hash "" v) and (hash-set! h "" v) lost the entry
//          with NO error at all. The empty string is an ORDINARY key (a
//          missing field, the root namespace, a default tenant, a
//          (string-split "a\t\tb" "\t") field). A genuinely corrupt string
//          (index out of range) must RAISE, never no-op.
//
//   AC1: the hash CONSTRUCTOR keeps the "" key — hash-has-key? is #t,
//        hash-ref returns the value, hash-length counts the entry.
//   AC2: hash-set! keeps the "" key — the same three checks.
//   AC3: the "" key coexists with ordinary keys — the other entries still
//        read back and the length counts both (no table corruption).
//   AC4: a genuinely corrupt string key (index out of range) RAISES an
//        error from the hash constructor AND from hash-set! — never a
//        silent no-op.
//   AC5: the source cites #4631 at both sites and no longer skips /
//        refuses empty string keys.

#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::test::g_failed;
using aura::test::g_passed;

using aura::compiler::types::EvalValue;

static std::string read_source(const std::string& rel) {
    for (const auto& p : {rel, "../" + rel, "../../" + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// The window of ONE primitive: from its registration site to the next
// register_prim call.
static std::string prim_window(const std::string& src, const std::string& name) {
    const auto at = src.find("\"" + name + "\",");
    if (at == std::string::npos)
        return {};
    auto stop = src.find("register_prim(", at);
    if (stop == std::string::npos)
        stop = src.size();
    return src.substr(at, stop - at);
}

// ── AC1: the hash constructor keeps the "" key ──
static void ac1_hash_ctor_keeps_empty_key() {
    std::println("\n--- #4631 AC1: the hash constructor keeps the \"\" key ---");
    CompilerService cs;
    auto has = cs.eval(R"((hash-has-key? (hash "" 0 "a" 1) ""))");
    CHECK(has && is_bool(*has) && as_bool(*has), "AC1: hash-has-key? is #t for the \"\" key");
    auto v = cs.eval(R"((hash-ref (hash "" 0 "a" 1) "" 9))");
    CHECK(v && is_int(*v) && as_int(*v) == 0, "AC1: hash-ref returns the stored value");
    auto len = cs.eval(R"((hash-length (hash "" 0 "a" 1)))");
    CHECK(len && is_int(*len) && as_int(*len) == 2, "AC1: hash-length counts both keys");
    // The constructor with ONLY the empty key must not produce an empty table.
    auto only = cs.eval(R"((hash-length (hash "" 7)))");
    CHECK(only && is_int(*only) && as_int(*only) == 1, "AC1: (hash \"\" 7) has one entry");
}

// ── AC2: hash-set! keeps the "" key ──
static void ac2_hash_set_keeps_empty_key() {
    std::println("\n--- #4631 AC2: hash-set! keeps the \"\" key ---");
    CompilerService cs;
    auto v = cs.eval(R"((begin (define h (hash "a" 1)) (hash-set! h "" 5) (hash-ref h "" 9)))");
    CHECK(v && is_int(*v) && as_int(*v) == 5, "AC2: hash-set! stores the \"\" key");
    auto has =
        cs.eval(R"((begin (define h (hash "a" 1)) (hash-set! h "" 5) (hash-has-key? h "")))");
    CHECK(has && is_bool(*has) && as_bool(*has), "AC2: hash-has-key? is #t after hash-set!");
    auto len = cs.eval(R"((begin (define h (hash "a" 1)) (hash-set! h "" 5) (hash-length h)))");
    CHECK(len && is_int(*len) && as_int(*len) == 2, "AC2: hash-length counts both keys");
    // The empty-string table on its own (the issue's second repro).
    auto empty_only = cs.eval(R"((begin (define e (hash)) (hash-set! e "" 5) (hash-ref e "" 9)))");
    CHECK(empty_only && is_int(*empty_only) && as_int(*empty_only) == 5,
          "AC2: (hash-set! (hash) \"\" 5) stores the key");
}

// ── AC3: the "" key coexists with ordinary keys ──
static void ac3_empty_key_coexists() {
    std::println("\n--- #4631 AC3: the \"\" key coexists with ordinary keys ---");
    CompilerService cs;
    auto a = cs.eval(R"((hash-ref (hash "" 0 "a" 1 "b" 2) "a" 9))");
    CHECK(a && is_int(*a) && as_int(*a) == 1, "AC3: the \"a\" key still reads back");
    auto b = cs.eval(R"((hash-ref (hash "" 0 "a" 1 "b" 2) "b" 9))");
    CHECK(b && is_int(*b) && as_int(*b) == 2, "AC3: the \"b\" key still reads back");
    auto len = cs.eval(R"((hash-length (hash "" 0 "a" 1 "b" 2)))");
    CHECK(len && is_int(*len) && as_int(*len) == 3, "AC3: hash-length counts all three keys");
    // The aura-memory scenario: a per-tenant table where the default
    // tenant's name is "".
    auto tenant = cs.eval(R"((begin
                             (define t (hash))
                             (hash-set! t "" 1)
                             (hash-set! t "other" 2)
                             (+ (hash-ref t "" 0) (hash-ref t "other" 0))))");
    CHECK(tenant && is_int(*tenant) && as_int(*tenant) == 3,
          "AC3: the default-tenant (\"\") entry survives next to a named tenant");
    // Removing the empty key must work too (the key is really in the table).
    auto removed = cs.eval(R"((begin
                               (define h (hash "" 0 "a" 1))
                               (hash-remove! h "")
                               (hash-has-key? h "")))");
    CHECK(removed && is_bool(*removed) && !as_bool(*removed),
          "AC3: hash-remove! removes the \"\" key");
}

// ── AC4: a corrupt string key RAISES (never a silent no-op) ──
static void ac4_corrupt_string_key_raises() {
    std::println("\n--- #4631 AC4: a corrupt string key RAISES ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto hash_fn = ev.primitives().lookup("hash");
    auto set_fn = ev.primitives().lookup("hash-set!");
    CHECK(hash_fn.has_value() && set_fn.has_value(), "AC4: the hash primitives are registered");
    if (!hash_fn || !set_fn)
        return;
    // A string index far past the end of the string heap is the #2652
    // corruption case: the key can never be a real string.
    const auto corrupt = make_string(std::uint64_t{1000000000});
    {
        std::array<EvalValue, 2> args{corrupt, make_int(7)};
        const EvalValue r = (*hash_fn)(std::span<const EvalValue>(args));
        CHECK(is_error(r), "AC4: the hash constructor RAISES on a corrupt string key");
    }
    {
        auto h = cs.eval(R"((hash))");
        CHECK(h.has_value(), "AC4: hash created");
        if (!h)
            return;
        std::array<EvalValue, 3> args{*h, corrupt, make_int(7)};
        const EvalValue r = (*set_fn)(std::span<const EvalValue>(args));
        CHECK(is_error(r), "AC4: hash-set! RAISES on a corrupt string key");
        // The table must be untouched (the write never happened).
        auto len = cs.eval(R"((hash-length (hash)))");
        CHECK(len && is_int(*len) && as_int(*len) == 0,
              "AC4: the rejected write left the table empty");
    }
}

// ── AC5: source cite ──
static void ac5_source_cite() {
    std::println("\n--- #4631 AC5: source cite ---");
    const auto vec = read_source("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(!vec.empty(), "AC5: the vector/hash source is readable");
    const auto ctor = prim_window(vec, "hash");
    const auto setter = prim_window(vec, "hash-set!");
    CHECK(ctor.find("#4631") != std::string::npos, "AC5: the hash constructor cites #4631");
    CHECK(setter.find("#4631") != std::string::npos, "AC5: hash-set! cites #4631");
    // The old silent-drop guards must be gone from BOTH sites.
    CHECK(ctor.find("string_heap[ki].empty()") == std::string::npos,
          "AC5: the hash constructor no longer skips empty string keys");
    CHECK(setter.find("string_heap[bi].empty()") == std::string::npos,
          "AC5: hash-set! no longer refuses empty string keys");
    // The corruption guard is still there — it guards the INDEX, and it raises.
    CHECK(ctor.find("corrupt string key") != std::string::npos,
          "AC5: the hash constructor raises on a corrupt string key");
    CHECK(setter.find("corrupt string key") != std::string::npos,
          "AC5: hash-set! raises on a corrupt string key");
}

} // namespace

int main() {
    std::println("=== Issue #4631: the empty string is an ordinary hash key ===");
    ac1_hash_ctor_keeps_empty_key();
    ac2_hash_set_keeps_empty_key();
    ac3_empty_key_coexists();
    ac4_corrupt_string_key_raises();
    ac5_source_cite();
    std::println("\n=== #4631: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
