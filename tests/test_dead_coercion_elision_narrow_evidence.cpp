// test_dead_coercion_elision_narrow_evidence.cpp — Issue #799:
// DeadCoercionElimination + narrow_evidence CastOp elision observability
// (refines #796/#795/#794; non-duplicative with #687 dead-coercion-elim-stats,
// #629 coercion-zerooverhead).
//
//   - AC1:  query:dead-coercion-elision-stats reachable (schema 799)
//   - AC2:  elided-casts bumps on direct path
//   - AC3:  evidence-hit-rate derived from evidence hits
//   - AC4:  narrowing-stable-paths bumps on direct path
//   - AC5:  runtime-check-savings bumps on direct path
//   - AC6:  DCE Rule 6 narrow_evidence elision (no CastOp residual)
//   - AC7:  metrics monotonic after DCE + bump matrix
//   - AC8:  query regression (#687 elim-stats, #629 zerooverhead-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_799_detail {

using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:dead-coercion-elision-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t elided_casts(CompilerService& cs) {
    return stat_int(cs, "elided-casts");
}
static std::int64_t evidence_hit_rate(CompilerService& cs) {
    return stat_int(cs, "evidence-hit-rate");
}
static std::int64_t narrowing_stable_paths(CompilerService& cs) {
    return stat_int(cs, "narrowing-stable-paths");
}
static std::int64_t runtime_check_savings(CompilerService& cs) {
    return stat_int(cs, "runtime-check-savings");
}

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

static IRModule make_narrow_cast_module() {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = 16});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {2, 1, 0, 0};
    cast.type_id = 1;
    cast.narrow_evidence = 4;
    func.blocks[0].instructions = {
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::Local, {1, 0, 0, 0}, 1, 1},
        cast,
        {IROpcode::Return, {2, 0, 0, 0}, 0, 0},
    };
    return mod;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:dead-coercion-elision-stats (schema 799) ---");
    auto h = cs.eval("(query:dead-coercion-elision-stats)");
    CHECK(h && is_hash(*h), "dead-coercion-elision-stats returns hash");
    CHECK(stat_int(cs, "schema") == 799, "schema == 799");
    CHECK(elided_casts(cs) >= 0, "elided-casts non-negative");
    CHECK(evidence_hit_rate(cs) >= 0, "evidence-hit-rate non-negative");
    CHECK(narrowing_stable_paths(cs) >= 0, "narrowing-stable-paths non-negative");
    CHECK(runtime_check_savings(cs) >= 0, "runtime-check-savings non-negative");

    std::println("\n--- AC2: elided-casts bumps on direct path ---");
    const auto e0 = elided_casts(cs);
    cs.evaluator().bump_dead_coercion_elision_elided_casts(2);
    CHECK(elided_casts(cs) == e0 + 2, "elided-casts bumps by exactly 2");

    std::println("\n--- AC3: evidence-hit-rate derived ---");
    const auto rate0 = evidence_hit_rate(cs);
    cs.evaluator().bump_dead_coercion_elision_evidence_hits(3);
    const auto rate1 = evidence_hit_rate(cs);
    CHECK(rate1 >= rate0, "evidence-hit-rate monotonic after evidence bumps");

    std::println("\n--- AC4: narrowing-stable-paths bumps on direct path ---");
    const auto n0 = narrowing_stable_paths(cs);
    cs.evaluator().bump_dead_coercion_elision_narrowing_stable_paths();
    CHECK(narrowing_stable_paths(cs) == n0 + 1, "narrowing-stable-paths bumps by 1");

    std::println("\n--- AC5: runtime-check-savings bumps on direct path ---");
    const auto s0 = runtime_check_savings(cs);
    cs.evaluator().bump_dead_coercion_elision_runtime_check_savings(2);
    CHECK(runtime_check_savings(cs) == s0 + 2, "runtime-check-savings bumps by exactly 2");

    std::println("\n--- AC6: DCE Rule 6 narrow_evidence elision ---");
    auto mod = make_narrow_cast_module();
    CHECK(count_cast_ops(mod) == 1, "module starts with one CastOp");
    DeadCoercionEliminationPass dce(nullptr);
    dce.run(mod);
    CHECK(count_cast_ops(mod) == 0, "DCE Rule 6 eliminates narrow_evidence CastOp");
    CHECK(dce.narrow_evidence_hits() >= 1, "DCE reports narrow_evidence hit");

    std::println("\n--- AC7: metrics monotonic after DCE matrix ---");
    const auto ev7a = elided_casts(cs) + narrowing_stable_paths(cs) + runtime_check_savings(cs);
    cs.evaluator().bump_dead_coercion_elision_elided_casts();
    cs.evaluator().bump_dead_coercion_elision_narrowing_stable_paths();
    cs.evaluator().bump_dead_coercion_elision_runtime_check_savings();
    const auto ev7b = elided_casts(cs) + narrowing_stable_paths(cs) + runtime_check_savings(cs);
    CHECK(ev7b >= ev7a + 3, "aggregate elision counters monotonic");

    std::println("\n--- AC8: query regression ---");
    auto elim687 = cs.eval("(query:dead-coercion-elim-stats)");
    auto z629 = cs.eval("(query:dead-coercion-zerooverhead-stats)");
    CHECK(elim687 && is_hash(*elim687), "dead-coercion-elim-stats regression (#687)");
    CHECK(z629 && is_hash(*z629), "dead-coercion-zerooverhead-stats regression (#629)");
}

} // namespace aura_issue_799_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_799_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}