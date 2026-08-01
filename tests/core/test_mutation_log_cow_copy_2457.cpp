// @category: unit
// @reason: Issue #2457 — FlatAST copy shares mutation_log_ / narrowing_log_
//          via shared_ptr COW; first mutate detaches.
//
//   AC1: copy shares log sizes (no deep-copy isolation until write)
//   AC2: mutating child log does not change parent (COW detach)
//   AC3: source cites #2457 + CowPmrVector + shared_ptr share on copy

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.ast;
import aura.core.mutation;

namespace {

using aura::ast::FlatAST;
using aura::ast::MutationRecord;
using aura::ast::MutationStatus;
using aura::ast::NarrowingRecord;
using aura::ast::NULL_NODE;
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

static MutationRecord make_rec(std::uint64_t id, std::uint32_t node) {
    MutationRecord r{};
    r.mutation_id = id;
    r.target_node = node;
    r.status = MutationStatus::Committed;
    r.operator_name = "test";
    return r;
}

static NarrowingRecord make_narrow(const char* var) {
    NarrowingRecord n{};
    n.var_name = var;
    n.predicate_src = "string?";
    n.refined_type_str = "String";
    n.if_node = NULL_NODE;
    return n;
}

} // namespace

int main() {
    std::println("=== Issue #2457: mutation_log_ / narrowing_log_ COW copy ===");

    // ── AC1: copy preserves log contents (shared or deep — visible equal) ──
    {
        std::println("\n--- #2457 AC1: copy preserves log contents ---");
        FlatAST parent;
        parent.all_mutations().push_back(make_rec(1, 0));
        parent.all_mutations().push_back(make_rec(2, 1));
        parent.all_narrowings().push_back(make_narrow("x"));
        parent.all_narrowings().push_back(make_narrow("y"));
        CHECK(parent.mutation_count() == 2, "AC1: parent mut count 2");
        CHECK(parent.narrowing_count() == 2, "AC1: parent narrow count 2");

        FlatAST child = parent;
        CHECK(child.mutation_count() == 2, "AC1: child mut count 2");
        CHECK(child.narrowing_count() == 2, "AC1: child narrow count 2");
        CHECK(child.all_mutations()[0].mutation_id == 1, "AC1: child mut id 1");
        CHECK(child.all_narrowings()[1].var_name == "y", "AC1: child narrow y");
    }

    // ── AC2: COW isolation on first write ──────────────────────────
    {
        std::println("\n--- #2457 AC2: mutate child detaches from parent ---");
        FlatAST parent;
        parent.all_mutations().push_back(make_rec(10, 0));
        parent.all_mutations().push_back(make_rec(11, 0));
        parent.all_narrowings().push_back(make_narrow("a"));

        FlatAST child = parent;
        CHECK(child.mutation_count() == 2, "AC2: shared size before write");
        CHECK(parent.mutation_count() == 2, "AC2: parent still 2");

        // Write on child — must COW-detach mutation log.
        child.all_mutations().push_back(make_rec(12, 0));
        CHECK(child.mutation_count() == 3, "AC2: child mut 3 after push");
        CHECK(parent.mutation_count() == 2, "AC2: parent mut still 2 (isolated)");

        // Write on parent — must not change child.
        parent.all_mutations().push_back(make_rec(13, 0));
        CHECK(parent.mutation_count() == 3, "AC2: parent mut 3");
        CHECK(child.mutation_count() == 3, "AC2: child mut still 3");

        // Narrowing log isolation
        child.all_narrowings().push_back(make_narrow("b"));
        CHECK(child.narrowing_count() == 2, "AC2: child narrow 2");
        CHECK(parent.narrowing_count() == 1, "AC2: parent narrow still 1");

        // Copy-assign path also COW-shares then detaches
        FlatAST assigned;
        assigned = parent;
        CHECK(assigned.mutation_count() == parent.mutation_count(), "AC2: assign copies size");
        assigned.all_mutations().clear();
        CHECK(assigned.mutation_count() == 0, "AC2: assigned cleared");
        CHECK(parent.mutation_count() == 3, "AC2: parent intact after assign clear");
    }

    // ── AC3: source cite Option A ──────────────────────────────────
    {
        std::println("\n--- #2457 AC3: source cites CowPmrVector + #2457 ---");
        auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("Issue #2457") != std::string::npos, "AC3: cites #2457");
        CHECK(ast.find("CowPmrVector") != std::string::npos, "AC3: CowPmrVector type");
        CHECK(ast.find("shared_ptr") != std::string::npos &&
                  ast.find("mutation_log_") != std::string::npos,
              "AC3: shared_ptr COW for logs");
        CHECK(ast.find("CowPmrVector<MutationRecord> mutation_log_") != std::string::npos,
              "AC3: mutation_log_ is CowPmrVector");
        CHECK(ast.find("CowPmrVector<NarrowingRecord> narrowing_log_") != std::string::npos,
              "AC3: narrowing_log_ is CowPmrVector");
        // Copy path documents share (not deep vector copy of logs alone)
        CHECK(ast.find("share COW logs") != std::string::npos ||
                  ast.find("shared_ptr COW") != std::string::npos,
              "AC3: copy documents share/COW");
    }

    std::println("\n=== #2457 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
