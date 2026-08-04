// @category: unit
// @reason: Issue #2394 — last_validated_generation is concurrent-safe
// (CopyableAtomicU16) under concurrent validate_with_provenance.
//
//   AC1: 4 threads validate_with_provenance on same ref — no race (TSAN)
//   AC2: load() returns valid uint16 (matches flat generation after ok)
//   AC3: wire round-trip + copy still work (assignment / conversion)
//   AC4: hot path store is relaxed atomic (source-cite; no heavy bench)
//   AC5: this test + CMake + build.py gate

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
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

// ── AC1/AC2: concurrent validate_with_provenance ──
static void ac1_ac2_concurrent_validate() {
    std::println("\n--- #2394 AC1/AC2: 4 threads concurrent validate_with_provenance ---");
    FlatAST flat;
    const NodeId id = flat.add_literal(42);
    FlatAST::StableNodeRef ref = flat.make_safe_ref(id);
    CHECK(ref.id == id, "AC1: ref id");
    CHECK(flat.is_valid(ref), "AC1: ref valid");

    constexpr int kThreads = 4;
    constexpr int kIters = 5000;
    std::atomic<int> ok_count{0};
    std::vector<std::thread> thr;
    thr.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        thr.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                if (ref.validate_with_provenance(flat))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : thr)
        th.join();

    const auto expected = kThreads * kIters;
    std::println("  ok_count={} expected={}", ok_count.load(), expected);
    CHECK(ok_count.load() == expected, "AC1: all concurrent validates succeeded");

    // AC2: field is a valid generation (matches flat gen after successful validate).
    const auto lvg = ref.last_validated_generation.load(std::memory_order_relaxed);
    const auto gen = flat.generation();
    std::println("  last_validated={} flat.gen={}", lvg, gen);
    CHECK(lvg == gen, "AC2: last_validated_generation == flat.generation()");
    CHECK(lvg != 0 || gen == 0, "AC2: non-torn store (matches live gen)");
}

// ── AC3: copy + assignment + wire-compatible assignment ──
static void ac3_copy_and_assign() {
    std::println("\n--- #2394 AC3: copy / assign / conversion ---");
    FlatAST flat;
    const NodeId id = flat.add_literal(7);
    FlatAST::StableNodeRef a = flat.make_safe_ref(id);
    CHECK(a.validate_with_provenance(flat), "AC3: validate ok");
    const auto g = static_cast<std::uint16_t>(a.last_validated_generation);

    FlatAST::StableNodeRef b = a; // copy ctor
    CHECK(static_cast<std::uint16_t>(b.last_validated_generation) == g,
          "AC3: copy preserves last_validated");

    FlatAST::StableNodeRef c{};
    c = a; // copy assign
    CHECK(static_cast<std::uint16_t>(c.last_validated_generation) == g,
          "AC3: assign preserves last_validated");

    c.last_validated_generation = 123; // operator=(uint16)
    CHECK(static_cast<std::uint16_t>(c.last_validated_generation) == 123,
          "AC3: assign from uint16");
}

// ── AC4/AC5: source-cite + gate ──
static void ac4_ac5_source_and_gate() {
    std::println("\n--- #2394 AC4/AC5: source-cite + gate ---");
    const auto ixx = read_file("src/core/ast.ixx");
    const auto stab = read_file("src/core/ast_stability.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter =
        read_file("scripts/coverage/checks/check_last_validated_generation_atomic_2394.py");

    CHECK(ixx.find("Issue #2394") != std::string::npos ||
              ixx.find("CopyableAtomicU16") != std::string::npos,
          "AC4: CopyableAtomicU16 / #2394 in ast.ixx");
    CHECK(ixx.find("CopyableAtomicU16") != std::string::npos, "AC4: CopyableAtomicU16 type");
    CHECK(ixx.find("last_validated_generation") != std::string::npos, "AC4: field present");
    CHECK(stab.find("Issue #2394") != std::string::npos, "AC4: ast_stability cites #2394");
    CHECK(stab.find("memory_order_relaxed") != std::string::npos,
          "AC4: relaxed atomic store in validate_with_provenance");
    CHECK(stab.find("last_validated_generation.store") != std::string::npos ||
              stab.find("store(ast.generation()") != std::string::npos,
          "AC4: store() used for concurrent write");
    CHECK(cmake.find("test_last_validated_generation_atomic") != std::string::npos, "AC5: CMake");
    CHECK(build.find("check_last_validated_generation_atomic_2394") != std::string::npos ||
              build.find("cmd_last_validated_generation_atomic_coverage") != std::string::npos,
          "AC5: build.py gate");
    CHECK(!linter.empty(), "AC5: coverage linter present");
}

} // namespace

int run_test_last_validated_generation_atomic() {
    std::println("=== Issue #2394: last_validated_generation atomic ===");
    ac1_ac2_concurrent_validate();
    ac3_copy_and_assign();
    ac4_ac5_source_and_gate();
    std::println("\n=== #2394 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_last_validated_generation_atomic();
}
#endif
