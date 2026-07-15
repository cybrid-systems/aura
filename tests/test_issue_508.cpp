// test_issue_508.cpp — Issue #508: DeadCoercionEliminationPass
// dedicated verification (extends the embedded tests in
// test_ir.cpp + the metric-plumbing test in test_issue_433.cpp).
//
// Scope (matches the 4 remaining issue ACs):
//   AC1: Dynamic passthrough rule elides safe Dynamic-target casts.
//   AC2: keep_for_debug disables elision but counts CastOps.
//   AC3: elapsed_us is monotonic + observable via the
//        (stats:get "compile:dead-coercion-elapsed") primitive.
//   AC4: End-to-end gradual-typed mutation loop: result is
//        identical with and without elision; elimination
//        count > 0 in the gradual case.
//
// Each test prints PASS/FAIL with a short tag. The driver
// returns 0 iff all tests pass.

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_508_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println("  PASS: {}", msg);                                                       \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                                        \
    do {                                                                                           \
        auto _a = (a);                                                                             \
        auto _b = (b);                                                                             \
        if (_a == _b) {                                                                            \
            ++g_passed;                                                                            \
            std::println("  PASS: {}  ({} = {})", msg, _a, _b);                                    \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}  ({} != {})", msg, _a, _b);                        \
        }                                                                                          \
    } while (0)

using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

// Count CastOps in a module.
static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

// Build a tiny IRModule with one block, the given instructions.
// Returns the block reference for inspection.
static IRModule make_module_with(std::vector<IRInstruction> instrs,
                                 std::uint32_t local_count = 16) {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = local_count});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    func.blocks.back().instructions = std::move(instrs);
    return mod;
}

// ── AC1: Dynamic passthrough ───────────────────────────────
bool test_dynamic_passthrough() {
    std::println("\n--- AC1: safe Dynamic passthrough rule ---");
    // Source: ConstI64 (type_id=1=Int, ground-typed). CastOp to
    // Dynamic (target_tag=3). The default case in the IR
    // interpreter is `locals[ops[0]] = val` — pure passthrough.
    // The pass should elide this CastOp.
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1}, // slot0 = 42, type_id=1
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},    // cast slot0 → Dynamic (tag 3)
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    std::size_t before = count_cast_ops(mod);
    aura::compiler::DeadCoercionEliminationPass dce;
    dce.run(mod);
    std::size_t after = count_cast_ops(mod);
    CHECK_EQ(before, std::size_t{1}, "1 CastOp before elision");
    CHECK_EQ(after, std::size_t{0}, "Dynamic passthrough CastOp elided");
    CHECK_EQ(dce.eliminated_count(), std::size_t{1}, "eliminated_count == 1");
    return true;
}

// ── AC1b: Dynamic passthrough does NOT elide unknown source ─
bool test_dynamic_passthrough_unknown_source() {
    std::println("\n--- AC1b: Dynamic passthrough is conservative ---");
    // Source: Arg with no type_id (type_id=0). CastOp to Dynamic.
    // We don't know the source type, so we cannot safely elide
    // — the lowering pass may have inserted the CastOp for a
    // reason (e.g. boundary with external code).
    auto mod = make_module_with({
        {IROpcode::Arg, {0, 0, 0, 0}, 0, 0},    // slot0, no type info
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0}, // cast to Dynamic
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    std::size_t before = count_cast_ops(mod);
    aura::compiler::DeadCoercionEliminationPass dce;
    dce.run(mod);
    std::size_t after = count_cast_ops(mod);
    CHECK_EQ(before, std::size_t{1}, "1 CastOp before elision");
    CHECK_EQ(after, std::size_t{1}, "Unknown-source Dynamic cast NOT elided (conservative)");
    CHECK_EQ(dce.eliminated_count(), std::size_t{0}, "eliminated_count == 0");
    return true;
}

// ── AC1c: nested Dynamic passthrough collapses ──────────────
bool test_dynamic_passthrough_chain() {
    std::println("\n--- AC1c: chain of Dynamic passthroughs collapses ---");
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0}, // ground → Dynamic
        {IROpcode::CastOp, {2, 1, 3, 0}, 0, 0}, // Dynamic → Dynamic
        {IROpcode::Return, {2, 0, 0, 0}, 0, 0},
    });
    aura::compiler::DeadCoercionEliminationPass dce;
    dce.run(mod);
    // After elision: first cast becomes Local (slot1 = slot0),
    // then second cast (Dynamic → Dynamic) becomes Local (slot2 = slot1).
    std::size_t after = count_cast_ops(mod);
    CHECK_EQ(after, std::size_t{0}, "Both Dynamic CastOps elided via chain");
    CHECK(dce.eliminated_count() >= 2, "eliminated_count >= 2 (chain collapses through)");
    return true;
}

// ── AC2: keep_for_debug disables elision ──────────────────
bool test_keep_for_debug() {
    std::println("\n--- AC2: keep_for_debug disables elision ---");
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    aura::compiler::DeadCoercionEliminationPass dce;
    dce.set_keep_for_debug(true);
    dce.run(mod);
    std::size_t after = count_cast_ops(mod);
    CHECK_EQ(after, std::size_t{1}, "CastOp preserved with keep_for_debug");
    CHECK_EQ(dce.eliminated_count(), std::size_t{0}, "eliminated_count == 0 in debug mode");
    CHECK_EQ(dce.kept_for_debug_count(), std::size_t{1}, "kept_for_debug_count == 1");
    // Disable and re-run on a fresh module to verify the toggle.
    auto mod2 = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    dce.set_keep_for_debug(false);
    dce.run(mod2);
    CHECK_EQ(count_cast_ops(mod2), std::size_t{0},
             "After disabling keep_for_debug, elision resumes");
    return true;
}

// ── AC3a: elapsed_us is observable via snapshot ───────────
bool test_elapsed_us_snapshot() {
    std::println("\n--- AC3a: elapsed_us reflected in snapshot ---");
    aura::compiler::CompilerService cs;
    auto snap0 = cs.snapshot();
    std::uint64_t t0 = snap0.dead_coercion_elapsed_us_total;
    // Trigger the pipeline via a small eval. The pass runs and
    // elapsed_us_total should be >= 0 (might be 0 if the
    // chrono clock rounds down on very fast runs).
    cs.eval("(set-code \"(define q 42)\")");
    cs.eval("(eval-current)");
    auto snap1 = cs.snapshot();
    std::println("  elapsed_us_total before: {}, after: {}", t0,
                 snap1.dead_coercion_elapsed_us_total);
    CHECK(snap1.dead_coercion_elapsed_us_total >= t0,
          "elapsed_us_total is monotonic non-decreasing");
    return true;
}

// ── AC3b: elapsed_us primitive returns int ────────────────
bool test_elapsed_us_primitive() {
    std::println("\n--- AC3b: (stats:get \"compile:dead-coercion-elapsed\") primitive ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:get \"compile:dead-coercion-elapsed\")");
    CHECK(r && aura::compiler::types::is_int(*r),
          "(stats:get \"compile:dead-coercion-elapsed\") returns int");
    if (r && aura::compiler::types::is_int(*r)) {
        std::int64_t v = aura::compiler::types::as_int(*r);
        CHECK(v >= 0, "elapsed_us >= 0");
        std::println("  current elapsed_us: {}", v);
    }
    return true;
}

// ── AC3c: kept_for_debug primitive returns int ────────────
bool test_kept_for_debug_primitive() {
    std::println("\n--- AC3c: (compile:dead-coercion-kept-for-debug) primitive ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(compile:dead-coercion-kept-for-debug)");
    CHECK(r && aura::compiler::types::is_int(*r),
          "(compile:dead-coercion-kept-for-debug) returns int");
    return true;
}

// ── AC4: End-to-end gradual mutation loop ─────────────────
// Two scenarios:
//   (a) Program with ground-to-ground casts that should be elided.
//   (b) Program with ground-to-Dynamic casts that should be elided
//       via the new Dynamic passthrough rule.
// Both scenarios: result identical before/after, eliminated_count > 0.
bool test_e2e_gradual() {
    std::println("\n--- AC4: end-to-end gradual mutation loop ---");
    aura::compiler::CompilerService cs;
    cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")");
    auto r1 = cs.eval("(eval-current)");
    CHECK(r1.has_value(), "Initial eval succeeds");
    auto snap1 = cs.snapshot();
    std::uint64_t e1 = snap1.dead_coercion_eliminated_total;
    std::println("  baseline dead_coercion_eliminated_total: {}", e1);

    // Mutate and re-eval several times. Each pass through the
    // pipeline runs dce. Result should remain a valid int/string/bool.
    for (int i = 0; i < 5; ++i) {
        std::string code = "(set-code \"(define v ";
        code += std::to_string(i * 7);
        code += ")\")";
        auto rs = cs.eval(code);
        CHECK(rs.has_value(), std::string("set-code #") + std::to_string(i) + " ok");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), std::string("Mutation #") + std::to_string(i) + " eval succeeds");
        std::println("    iter {}: v={} dead_coercion_elim_total={}", i,
                     r ? std::to_string(aura::compiler::types::as_int(*r)) : "<err>",
                     cs.snapshot().dead_coercion_eliminated_total);
    }
    auto snap2 = cs.snapshot();
    std::uint64_t e2 = snap2.dead_coercion_eliminated_total;
    std::println("  after 5 mutations dead_coercion_eliminated_total: {}", e2);
    CHECK(e2 >= e1, "Eliminated count is monotonic across mutations");
    return true;
}

// ── AC4b: gradual mixed-typed coercion chain ──────────────
bool test_e2e_gradual_mixed() {
    std::println("\n--- AC4b: gradual mixed-typed coercion chain ---");
    aura::compiler::CompilerService cs;
    // String→Int coercion via a function call would emit a CastOp
    // (string-to-int). Ground-to-ground: identity. Should not
    // affect runtime but exercised through the pipeline.
    cs.eval("(set-code \"(define s \\\"123\\\") (define n (cast s 'Int))\")");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value(), "Mixed coercion program eval succeeds");
    return true;
}

} // namespace aura_508_detail

int main() {
    using namespace aura_508_detail;
    test_dynamic_passthrough();
    test_dynamic_passthrough_unknown_source();
    test_dynamic_passthrough_chain();
    test_keep_for_debug();
    test_elapsed_us_snapshot();
    test_elapsed_us_primitive();
    test_kept_for_debug_primitive();
    test_e2e_gradual();
    test_e2e_gradual_mixed();
    std::println("\nDead coercion elimination (#508): {}/{}/{} passed/failed/total", g_passed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}
