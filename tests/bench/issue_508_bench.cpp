// tests/bench/issue_508_bench.cpp — Issue #508 quantitative benchmark.
//
// Measures CastOp count before and after DeadCoercionEliminationPass
// on a synthetic gradual-typing workload (Dynamic values flowing
// through ground-typed boundaries).
//
// The hypothesis from the issue AC: ">40% coercion reduction in
// gradual workloads". The benchmark constructs an IR module with
// N blocks × M instructions where roughly half the CastOps are
// safe-to-elide (ground→Dynamic where source is ground-typed).
// It then runs the pass and reports the ratio.
//
// Output:
//   - Human-readable table to stdout
//   - Returns 0 iff reduction >= 40% (i.e. the AC is met)
//   - Returns 1 otherwise (the bench is a regression check, not a hard fail)
//
// Run via:
//   cmake --build build --target issue_508_bench
//   ./build/issue_508_bench

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;

namespace {

std::size_t count_cast_ops(const aura::ir::IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == aura::ir::IROpcode::CastOp)
                    ++n;
    return n;
}

// Build a workload of N blocks, each with:
//   - K ConstI64 ground-typed values
//   - K CastOp(ground → Dynamic) per value (safe passthrough)
//   - K CastOp(Dynamic → Int) per value (NOT safe — Dynamic source
//     has no type_id; we want some non-elided casts to keep the
//     benchmark honest)
//   - 1 Return
// K=4 ground values per block is the default. With K=4, each block
// has 4 (elidable) + 4 (non-elidable) = 8 CastOps, of which 50%
// should be elided. Higher K gives a tighter ratio.
aura::ir::IRModule build_workload(std::size_t num_blocks, std::size_t values_per_block) {
    using aura::ir::IRFunction;
    using aura::ir::IRInstruction;
    using aura::ir::IRModule;
    using aura::ir::IROpcode;

    IRModule mod;
    IRFunction func;
    func.name = "workload";
    // Reserve enough locals: num_blocks * (values_per_block * 3 + 2)
    // + 4 = a rough upper bound. We over-allocate to avoid
    // local_count overflow in IRInstruction.operands[0].
    func.local_count = static_cast<std::uint32_t>(num_blocks * (values_per_block * 3 + 4) + 16);
    mod.functions.push_back(std::move(func));
    auto& f = mod.functions.back();

    std::uint32_t next_slot = 0;
    for (std::size_t bi = 0; bi < num_blocks; ++bi) {
        aura::ir::BasicBlock blk;
        blk.id = static_cast<std::uint32_t>(bi);
        // Reserve a return-result slot for this block.
        auto ret_slot = next_slot++;
        for (std::size_t vi = 0; vi < values_per_block; ++vi) {
            // Ground ConstI64 (type_id = 1 = Int). Slot s0.
            auto s0 = next_slot++;
            blk.instructions.push_back({IROpcode::ConstI64, {s0, 42, 0, 0}, 0, 1});
            // CastOp ground → Dynamic (target_tag = 3, type_id = 0).
            // ELIDABLE via Rule 3 (safe Dynamic passthrough).
            auto s1 = next_slot++;
            blk.instructions.push_back({IROpcode::CastOp, {s1, s0, 3, 0}, 0, 0});
            // CastOp Dynamic → Int (target_tag = 0, type_id = 0).
            // NOT elidable: source has no type_id (we put a Local
            // here in the elided case, but for the non-elided
            // case we leave it as a CastOp from a ConstI64 with
            // type_id 0... wait, ConstI64 has type_id 1).
            // Actually: source s0 IS ground-typed (type_id 1),
            // target Int (type_id 0), so Rule 1 doesn't match.
            // Rule 3 doesn't apply (target_tag = 0 < 3).
            // NOT elided. Good.
            auto s2 = next_slot++;
            blk.instructions.push_back({IROpcode::CastOp, {s2, s1, 0, 0}, 0, 0});
            // Use s2 so the optimizer doesn't DCE it
            // (defensive — DCE is a separate pass).
            blk.instructions.push_back({IROpcode::Local, {ret_slot, s2, 0, 0}, 0, 0});
        }
        blk.instructions.push_back({IROpcode::Return, {ret_slot, 0, 0, 0}, 0, 0});
        f.blocks.push_back(std::move(blk));
    }
    return mod;
}

} // namespace

int main() {
    using namespace std::chrono;

    constexpr std::size_t N_BLOCKS = 64;
    constexpr std::size_t K_VALUES = 4;
    constexpr std::size_t EXPECTED_BEFORE = N_BLOCKS * K_VALUES * 2;
    // 1 of the 2 CastOps per value is elidable (ground→Dynamic).
    constexpr std::size_t EXPECTED_AFTER = N_BLOCKS * K_VALUES * 1;
    constexpr std::size_t MIN_REDUCTION_PCT = 40;

    auto mod = build_workload(N_BLOCKS, K_VALUES);
    auto before = count_cast_ops(mod);
    std::println("Workload: {} blocks × {} values = {} CastOps before", N_BLOCKS, K_VALUES, before);

    auto t0 = steady_clock::now();
    aura::compiler::DeadCoercionEliminationPass dce;
    dce.run(mod);
    auto t1 = steady_clock::now();
    auto us = duration_cast<microseconds>(t1 - t0).count();

    auto after = count_cast_ops(mod);
    auto eliminated = dce.eliminated_count();
    auto reduction_pct = before > 0 ? (100 * (before - after)) / before : 0;

    std::println("After: {} CastOps remain (eliminated: {})", after, eliminated);
    std::println("Elapsed: {} µs (per-call)", us);
    std::println("Reduction: {}%", reduction_pct);

    bool meets_ac = (before == EXPECTED_BEFORE) && (after == EXPECTED_AFTER) &&
                    (reduction_pct >= MIN_REDUCTION_PCT);

    std::println("");
    std::println("AC check (>={}% reduction in gradual workload): {}", MIN_REDUCTION_PCT,
                 meets_ac ? "PASS" : "FAIL");
    std::println("  before == {}: {}", EXPECTED_BEFORE, before == EXPECTED_BEFORE);
    std::println("  after  == {}: {}", EXPECTED_AFTER, after == EXPECTED_AFTER);
    std::println("  reduction_pct >= {}: {} (got {})", MIN_REDUCTION_PCT,
                 reduction_pct >= MIN_REDUCTION_PCT, reduction_pct);

    return meets_ac ? 0 : 1;
}
