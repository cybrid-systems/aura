import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.frontend;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_interpreter;
import aura.compiler.compute_kind;
import aura.compiler.arity;
import aura.compiler.service;
import aura.compiler.pass_manager;

// ── Memory pool tests (arena stats + arena group) ─────────────
bool test_arena_stats() {
    aura::ast::ASTArena arena(4096);

    // Allocate small objects (go through SmallObjectPool)
    auto* i = arena.create<int>(42);
    auto* d = arena.create<double>(3.14);
    auto* c = arena.create<char>('X');

    if (*i != 42 || *d != 3.14 || *c != 'X') return false;

    auto s = arena.stats();
    if (s.allocation_count < 3) return false;
    if (s.used < 3) return false;       // small pool has at least 3 slots
    if (s.peak_used < s.used) return false;

    // Allocate a large object (bypasses small pool, goes through main arena)
    struct Large { char data[128]; };
    auto* big = arena.create<Large>();
    if (!big) return false;
    auto s2 = arena.stats();
    if (s2.allocation_count < 4) return false;
    if (s2.used < 128) return false;

    arena.reset();
    auto after = arena.stats();
    if (after.used != 0) return false;
    if (after.capacity < 4096) return false;  // capacity includes 3MB small pool

    // Can still allocate after reset
    auto* x = arena.create<int>(99);
    if (*x != 99) return false;

    return true;
}

bool test_arena_group() {
    aura::ast::ArenaGroup group;

    auto& a1 = group.module_arena("module_a");
    auto* i1 = a1.create<int>(100);

    auto& a2 = group.module_arena("module_b");
    auto* i2 = a2.create<int>(200);

    if (*i1 != 100 || *i2 != 200) return false;
    if (group.count() != 2) return false;

    // Check per-module stats
    auto stats = group.module_stats();
    if (stats.size() != 2) return false;

    // Check aggregate
    auto total = group.total_stats();
    if (total.allocation_count < 2) return false;

    // Reset one module
    group.reset_module("module_a");
    if (group.module_arena("module_a").used() != 0) return false;
    if (group.module_arena("module_b").used() == 0) return false;

    // Reset all
    group.reset_all();
    if (group.module_arena("module_a").used() != 0) return false;
    if (group.module_arena("module_b").used() != 0) return false;

    // Format shouldn't crash
    auto fmt = total.format();
    if (fmt.empty()) return false;

    return true;
}

// Run compute-kind analysis on the top function and return a summary string
std::string check_compute_kind(const std::string& input) {
    aura::ast::ASTArena arena;
    auto pr = aura::parser::parse(input, arena);
    if (!pr.root) return "parse_fail";

    auto mod = aura::compiler::lower_to_ir(pr.root, arena);
    auto& top_func = mod.entry();

    aura::compiler::ComputeKindAnalysis analysis;
    auto result = analysis.analyze(top_func);
    if (!result.valid) return "invalid";

    // Build a compact string per instruction: K=Known, U=Unknown
    std::string summary;
    for (std::size_t bi = 0; bi < result.per_block_inst_kind.size(); ++bi) {
        if (bi > 0) summary += '|';
        for (auto k : result.per_block_inst_kind[bi]) {
            summary += (k == aura::compiler::ComputeKind::Known) ? 'K' : 'U';
        }
    }
    return summary;
}

int main() {
    aura::ast::ASTArena arena;
    aura::compiler::Evaluator evaluator;
    evaluator.set_arena(&arena);

    // Test cases: (input, expected)
    struct Test { std::string input; std::int64_t expected; };
    Test tests[] = {
        // Literals and arithmetic
        {"42", 42},
        {"(+ 1 2)", 3},
        {"(* 2 3)", 6},
        {"(+ 1 (* 2 3))", 7},
        {"(- 5 3)", 2},
        {"(/ 6 2)", 3},

        // Comparisons
        {"(= 1 1)", 1},
        {"(= 1 2)", 0},
        {"(< 1 2)", 1},
        {"(> 2 1)", 1},

        // Conditionals
        {"(if 1 42 0)", 42},
        {"(if 0 0 99)", 99},
        {"(if (> 3 2) 1 0)", 1},

        // Let bindings
        {"(let ((x 10)) x)", 10},
        {"(let ((x 10)) (let ((y 20)) (+ x y)))", 30},

        // Lambda + application (tree-walker style closures)
        {"((lambda (x) (* x 2)) 5)", 10},
        {"((lambda (x y) (+ x y)) 3 4)", 7},

        // Closure with free variable
        {"(let ((x 10)) ((lambda (y) (+ x y)) 5))", 15},

        // Nested let + lambda
        {"(let ((x 2)) (let ((f (lambda (y) (* x y)))) (f 3)))", 6},
    };

    int passed = 0, failed = 0;
    for (auto& t : tests) {
        arena.reset();
        auto pr = aura::parser::parse(t.input, arena);
        if (!pr.success || !pr.root) {
            std::println(std::cerr, "PARSE FAIL: {}", t.input);
            ++failed; continue;
        }

        auto ir_mod = aura::compiler::lower_to_ir(pr.root, arena);

        aura::compiler::IRInterpreter ir_interp(ir_mod, evaluator.primitives());
        auto result = ir_interp.execute();

        if (result && *result == t.expected) {
            ++passed;
        } else {
            std::print(std::cerr, "FAIL: {}", t.input);
            if (!result) std::print(std::cerr, " error: {}", result.error().message);
            else std::print(std::cerr, " got {} expected {}", *result, t.expected);
            std::println(std::cerr, "");
            ++failed;
        }
    }

    std::println("IR test: {}/{} passed", passed, passed + failed);

    if (failed > 0) return 1;

    // ── L2.3: compute-kind analysis tests ─────────────────────
    struct CKTest { std::string input; std::string desc; };
    CKTest ck_tests[] = {
        {"42", "literal"},
        {"(+ 1 2)", "add_const"},
        {"(let ((x 10)) x)", "let_known"},
        {"((lambda (x) (* x 2)) 5)", "lambda_call"},
    };

    int ck_passed = 0, ck_failed = 0;
    for (auto& t : ck_tests) {
        auto summary = check_compute_kind(t.input);
        if (summary.empty()) {
            std::println(std::cerr, "CK FAIL: {} empty summary", t.desc);
            ++ck_failed;
        } else if (summary.back() == 'U' && summary.find('K') != std::string::npos) {
            ++ck_passed;
            std::println(R"(CK OK:  {} → {} ("{}"))", t.desc, summary, t.input);
        } else {
            std::println(std::cerr, "CK FAIL: {} summary='{}'", t.desc, summary);
            ++ck_failed;
        }
    }

    std::println("Compute-kind test: {}/{}/{} passed/failed/total",
                 ck_passed, ck_failed, ck_passed + ck_failed);

    // ── L2.4: arity checking tests ───────────────────────────
    aura::compiler::ArityChecker arity_checker;

    struct ArityTest { std::string input; bool expect_error; std::string desc; };
    ArityTest arity_tests[] = {
        {"((lambda (x) (* x 2)) 5)", false, "correct_1arg"},
        {"((lambda (x y) (+ x y)) 3 4)", false, "correct_2arg"},
        {"((lambda (x) x) 1 2)", true, "wrong_arity_too_many"},
        {"((lambda (x y) (+ x y)) 5)", true, "wrong_arity_too_few"},
        {"((lambda () 42))", false, "zero_arg_ok"},
    };

    int arity_passed = 0, arity_failed = 0;
    for (auto& t : arity_tests) {
        arena.reset();
        auto pr = aura::parser::parse(t.input, arena);
        if (!pr.root) { std::println(std::cerr, "PARSE FAIL: {}", t.input); ++arity_failed; continue; }

        auto mod = aura::compiler::lower_to_ir(pr.root, arena);
        auto result = arity_checker.check(mod);

        bool got_error = result.has_error;
        if (got_error == t.expect_error) {
            ++arity_passed;
            if (got_error) {
                std::println(R"(ARITY OK: {} → {} (expected error: "{}"))",
                             t.desc, result.diagnostics[0].message, t.input);
            } else {
                std::println(R"(ARITY OK: {} → no error ("{}"))", t.desc, t.input);
            }
        } else {
            std::println(std::cerr, "ARITY FAIL: {} ({})", t.desc, t.input);
            if (got_error) {
                for (auto& d : result.diagnostics)
                    std::println(std::cerr, "  error: {}", d.message);
            } else {
                std::println(std::cerr, "  expected error but got none");
            }
            ++arity_failed;
        }
    }

    std::println("Arity test: {}/{}/{} passed/failed/total",
                 arity_passed, arity_failed, arity_passed + arity_failed);

    // ── Memory pool tests ───────────────────────────────────
    int mp_passed = 0, mp_failed = 0;
    if (test_arena_stats()) { std::println("ARENA OK: stats"); ++mp_passed; }
    else { std::println(std::cerr, "ARENA FAIL: stats"); ++mp_failed; }

    // ── A2.3: Pass Manager tests ─────────────────────────────
    // ── L2.5: Constant folding tests ────────────────────────────
    int cf_passed = 0, cf_failed = 0;

    auto test_const_fold = [&](const std::string& input,
                                std::int64_t expected,
                                std::size_t expected_folds,
                                const std::string& desc) {
        arena.reset();
        auto pr = aura::parser::parse(input, arena);
        if (!pr.root) { std::println(std::cerr, "CF PARSE FAIL: {}", input); ++cf_failed; return; }

        auto mod = aura::compiler::lower_to_ir(pr.root, arena);

        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ConstantFoldingWrap cf_pass;
        ck.run(mod);
        cf_pass.run(mod);

        aura::compiler::IRInterpreter interp(mod, evaluator.primitives());
        auto result = interp.execute();

        bool ok = result && *result == expected;
        if (ok && cf_pass.folded_count() == expected_folds) {
            std::println("CF OK: {} (folded {}, got {})", desc, cf_pass.folded_count(), *result);
            ++cf_passed;
        } else {
            std::println(std::cerr, "CF FAIL: {} (expected {} folds, got {}; result={} expected={})",
                         desc, expected_folds, cf_pass.folded_count(), *result, expected);
            ++cf_failed;
        }
    };

    test_const_fold("(+ 1 2)", 3, 1, "add_const");
    test_const_fold("(* 2 3)", 6, 1, "mul_const");
    test_const_fold("(+ 1 (* 2 3))", 7, 2, "nested_const");  // Mul(2,3) folds, Add(1,6) folds
    test_const_fold("(if 1 42 0)", 42, 2, "if_condition_const");  // both branches' Local copies from Knowns fold
    test_const_fold("(= 1 1)", 1, 1, "eq_const");
    test_const_fold("(> 3 2)", 1, 1, "gt_const");
    test_const_fold("(let ((x 10)) x)", 10, 1, "let_copy");  // Local from Known → folded
    // (let ((x 10)) (+ x 5)): Local(x_copy, x_slot) folds, Add(x_copy, 5) also folds → 2
    test_const_fold("(let ((x 10)) (+ x 5))", 15, 2, "let_add");
    // lambda call: caller's Local(arg_copy, ConstI64) folds, but lambda's Arg doesn't → 1
    test_const_fold("((lambda (x) (* x 2)) 5)", 10, 1, "lambda_arg_unknown");
    // (+ 1 (+ 2 3)): inner Add(2,3) folds, outer Add(1,5) folds → 2
    test_const_fold("(+ 1 (+ 2 3))", 6, 2, "nested_add");

    std::println("Constant-fold test: {}/{}/{} passed/failed/total",
                 cf_passed, cf_failed, cf_passed + cf_failed);

    // ── Pipeline tests (concept-based fold) ────────────────────
    int pm_passed = 0, pm_failed = 0;

    {
        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ArityWrap ar;

        aura::ast::ASTArena arena;
        auto pr = aura::parser::parse("(+ 1 2)", arena);
        auto mod = aura::compiler::lower_to_ir(pr.root, arena);

        ck.run(mod);
        ar.run(mod);

        if (!ar.has_error()) {
            std::println("PM OK: pipeline ran, arity no error");
            ++pm_passed;
        } else {
            std::println(std::cerr, "PM FAIL: arity false positive");
            ++pm_failed;
        }
    }

    {
        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ArityWrap ar;
        aura::compiler::ConstantFoldingWrap cf;

        aura::ast::ASTArena arena;
        auto pr = aura::parser::parse("((lambda (x) x) 1 2)", arena);
        auto mod = aura::compiler::lower_to_ir(pr.root, arena);

        ck.run(mod);
        ar.run(mod);
        cf.run(mod);

        if (ar.has_error()) {
            std::println("PM OK: arity error detected: {}", ar.result().diagnostics[0].message);
            ++pm_passed;
        } else {
            std::println(std::cerr, "PM FAIL: arity missed error");
            ++pm_failed;
        }
    }

    // Test fold expression pipeline (compile-time ordering)
    {
        aura::compiler::ComputeKindWrap ck;
        aura::compiler::ArityWrap ar;

        aura::ast::ASTArena arena;
        auto pr = aura::parser::parse("(+ 1 2)", arena);
        auto mod = aura::compiler::lower_to_ir(pr.root, arena);

        auto pipeline_ok = aura::compiler::run_pipeline(mod, ck, ar);
        if (pipeline_ok) {
            std::println("PM OK: run_pipeline fold works");
            ++pm_passed;
        } else {
            std::println(std::cerr, "PM FAIL: run_pipeline fold broken");
            ++pm_failed;
        }
    }

    std::println("Pipeline test: {}/{}/{} passed/failed/total",
                 pm_passed, pm_failed, pm_passed + pm_failed);

    if (test_arena_group()) { std::println("ARENA OK: group"); ++mp_passed; }
    else { std::println(std::cerr, "ARENA FAIL: group"); ++mp_failed; }

    // Demo: print stats from a real compilation
    {
        aura::compiler::CompilerService cs;
        cs.eval("(+ 1 2)");
        auto s = cs.arena().stats();
        std::println("ARENA DEMO: {}", s.format());
    }

    // SmallObjectPool tier test: verify proper size class routing
    {
        aura::ast::ASTArena arena(4096);
        struct S8  { char d[8];  };
        struct S24 { char d[24]; };
        struct S48 { char d[48]; };

        auto* a = arena.create<S8>();
        auto* b = arena.create<S24>();
        auto* c = arena.create<S48>();
        auto* d = arena.create<int>();

        if (a && b && c && d) {
            auto s = arena.stats();
            // 4 small objects, all from SmallObjectPool; large check
            struct Big { char d[256]; };
            arena.create<Big>();
            auto s2 = arena.stats();
            if (s2.allocation_count == 5 && s2.used >= 256) {
                std::println("ARENA OK: small-object tiers (16/32/64) + overflow");
                ++mp_passed;
            } else {
                std::println(std::cerr, "ARENA FAIL: tier routing (used={}, allocs={})",
                             s2.used, s2.allocation_count);
                ++mp_failed;
            }
        } else {
            std::println(std::cerr, "ARENA FAIL: small-object allocation null");
            ++mp_failed;
        }
    }

    // Multi-module arena demo
    {
        aura::compiler::CompilerService cs;
        auto& ma = cs.module_arena("kernel");
        ma.create<int>(42);
        auto& mb = cs.module_arena("driver");
        mb.create<double>(3.14);
        mb.create<double>(2.71);

        auto total = cs.memory_stats();
        std::println("ARENA MULTI: {} modules, total {}",
                     cs.module_memory_stats().size(), total.format());

        cs.reset_module("driver");
        auto after = cs.memory_stats();
        std::println("ARENA MULTI: after driver reset, total {}", after.format());

        if (cs.module_arena("kernel").used() > 0 &&
            cs.module_arena("driver").used() == 0) {
            std::println("ARENA OK: multi-module isolation");
            ++mp_passed;
        } else {
            std::println(std::cerr, "ARENA FAIL: multi-module isolation");
            ++mp_failed;
        }
    }

    std::println("Memory pool test: {}/{}/{} passed/failed/total",
                 mp_passed, mp_failed, mp_passed + mp_failed);
    return (failed + ck_failed + arity_failed + mp_failed + pm_failed + cf_failed) > 0 ? 1 : 0;
}
