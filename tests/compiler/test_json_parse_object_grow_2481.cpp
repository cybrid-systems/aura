// @category: unit
// @reason: Issue #2481 — json-parse parse_object grows FlatHashTable so
//          N>5 keys are not silently dropped (fixed capacity 8).
//
//   AC1: 8-key object retains all keys
//   AC2: 16-key object retains all keys + hash-length
//   AC3: small object still works
//   AC4: source cites #2481 + grow/load-factor
//   AC5: gate wiring

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1: 8 keys (old capacity) ──
static void ac1_eight_keys() {
    std::println("\n--- #2481 AC1: 8-key object ---");
    CompilerService cs;
    auto r = cs.eval(
        R"((let ((h (json-parse "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8}")))
             (list (hash-length h)
                   (hash-ref h "a") (hash-ref h "b") (hash-ref h "c") (hash-ref h "d")
                   (hash-ref h "e") (hash-ref h "f") (hash-ref h "g") (hash-ref h "h"))))");
    CHECK(r.has_value() && !is_error(*r), "AC1: eval ok");
    // Walk list: length then 8 values
    auto len_r = cs.eval(
        R"((hash-length (json-parse "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8}")))");
    CHECK(len_r.has_value() && is_int(*len_r) && as_int(*len_r) == 8, "AC1: hash-length 8");
    for (const char* k : {"a", "b", "c", "d", "e", "f", "g", "h"}) {
        auto expr =
            std::string(
                R"((hash-ref (json-parse "{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5,\"f\":6,\"g\":7,\"h\":8}") ")") +
            k + "\")";
        auto kv = cs.eval(expr);
        CHECK(kv.has_value() && is_int(*kv), std::string("AC1: key ") + k + " present");
    }
}

// ── AC2: 16 keys (forces at least one grow from 8) ──
static void ac2_sixteen_keys() {
    std::println("\n--- #2481 AC2: 16-key object ---");
    CompilerService cs;
    // Build JSON with keys k0..k15
    std::string json = "{";
    for (int i = 0; i < 16; ++i) {
        if (i)
            json += ',';
        json += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
    }
    json += '}';
    // Escape quotes for S-expr string literal
    std::string escaped;
    for (char c : json) {
        if (c == '"')
            escaped += "\\\"";
        else if (c == '\\')
            escaped += "\\\\";
        else
            escaped += c;
    }
    auto len_expr = std::string("(hash-length (json-parse \"") + escaped + "\"))";
    auto len_r = cs.eval(len_expr);
    CHECK(len_r.has_value() && is_int(*len_r) && as_int(*len_r) == 16, "AC2: hash-length 16");
    for (int i = 0; i < 16; ++i) {
        auto expr = std::string("(hash-ref (json-parse \"") + escaped + "\") \"k" +
                    std::to_string(i) + "\")";
        auto kv = cs.eval(expr);
        CHECK(kv.has_value() && is_int(*kv) && as_int(*kv) == i,
              std::string("AC2: k") + std::to_string(i) + " present");
    }
}

// ── AC3: small object ──
static void ac3_small() {
    std::println("\n--- #2481 AC3: small object ---");
    CompilerService cs;
    auto r = cs.eval(R"((hash-ref (json-parse "{\"x\":42}") "x"))");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42, "AC3: single key");
    auto r2 = cs.eval(R"((hash-length (json-parse "{}")))");
    CHECK(r2.has_value() && is_int(*r2) && as_int(*r2) == 0, "AC3: empty object");
}

// ── AC4: source ──
static void ac4_source() {
    std::println("\n--- #2481 AC4: source grow ---");
    auto src = read_file("src/compiler/evaluator_primitives_json.cpp");
    CHECK(!src.empty(), "AC4: read json primitives");
    CHECK(src.find("Issue #2481") != std::string::npos, "AC4: cites #2481");
    auto pos = src.find("auto parse_object");
    CHECK(pos != std::string::npos, "AC4: parse_object present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 9000);
        CHECK(win.find("grow_object_table") != std::string::npos, "AC4: grow_object_table");
        CHECK(win.find("size * 10") != std::string::npos ||
                  win.find("capacity * 7") != std::string::npos,
              "AC4: load factor 0.7");
        CHECK(win.find("create(8)") != std::string::npos, "AC4: still starts at 8");
        CHECK(win.find("hash table grow failed") != std::string::npos, "AC4: fail-loud OOM");
        CHECK(win.find("make_primitive_error") != std::string::npos, "AC4: PRIM_ERROR path");
    }
    CHECK(src.find("json_object_key_hash") != std::string::npos, "AC4: shared key hash");
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2481 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_json_parse_object_grow_2481") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_json_parse_object_grow_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_json_parse_object_grow_2481") != std::string::npos, "AC5: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_json_parse_object_grow_2481.py").empty(),
          "AC5: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2481: json-parse object hash grow ===");
    ac1_eight_keys();
    ac2_sixteen_keys();
    ac3_small();
    ac4_source();
    ac5_gate();
    std::println("\n=== #2481 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
