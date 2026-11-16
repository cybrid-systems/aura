// @category: integration
// @reason: Issue #2259 — 64-bit tagged Value Encoding hot-path contracts +
// zero-overhead tagged dispatch for eval_flat / apply / ir_executor.
//
//   AC1: Pure is_* (is_fixnum_hot / is_int) match classify; single low2 path
//   AC2: as_int / pure arithmetic roundtrip (parity microbench proxy)
//   AC3: AURA_HOT_CONTRACT on as_* (debug fail-closed; release elided)
//   AC4: value_tag_hot_path_total + schema-2259 metrics
//   AC5: Semantic parity — existing is_int/as_int behavior under eval
//   AC6 (#4524): fixnum-first single-branch hot tests match the cold
//   classifier (is_fixnum_hot bit0 form; is_float_hot low8 kind form)
//   AC7 (#4524): interpreter float micro-loop — literal intern + TCO
//   backedge float-local step fast path: aura_alloc_float count over a
//   named-let float loop drops (~2/iter → ~1/iter), results identical
#include "test_harness.hpp"
#include "compiler/value_tags.h"
#include "compiler/shape_profiler.h"

#include <chrono>
#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.value;
import aura.compiler.service;

// Issue #4524: the float pool is the runtime-side box allocator
// (lib/aura_value_abi.h encoding; defined next to aura_float_ref).
// Pool-index probes measure aura_alloc_float traffic across an eval.
extern "C" std::int64_t aura_alloc_float(double d);

namespace {

using aura::compiler::CompilerService;
using aura::compiler::shape::inline_shape_of;
using aura::compiler::shape::SHAPE_INT;
namespace types = aura::compiler::types;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::classify_eval_value_tag;
using aura::compiler::types::classify_eval_value_tag_consteval;
using aura::compiler::types::EvalValueTag;
using aura::compiler::types::is_error;
using aura::compiler::types::is_fixnum_hot;
using aura::compiler::types::is_float;
using aura::compiler::types::is_float_hot;
using aura::compiler::types::is_int;
using aura::compiler::types::is_ref_hot;
using aura::compiler::types::is_string;
using aura::compiler::types::is_string_v2_hot;
using aura::compiler::types::kSpecialFalse;
using aura::compiler::types::kSpecialTrue;
using aura::compiler::types::kSpecialUnspecified;
using aura::compiler::types::kSpecialVoid;
using aura::compiler::types::make_float;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::note_value_tag_hot_path;
using aura::compiler::types::note_value_tag_stability;
using aura::compiler::types::tag_low2_hot;
using aura::compiler::types::value_tag_hot_path_total;
using aura::compiler::types::value_tag_hotpath_2259_wired;
using aura::compiler::types::value_tag_hotpath_zero_overhead_wired;
using aura::compiler::types::value_tag_stability_fixnum_total;
using aura::compiler::types::value_tag_stability_run_total;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:value-dispatch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_pure_tag_tests() {
    std::println("\n--- AC1: pure hot tag tests match classify ---");
    static_assert(is_fixnum_hot(0));
    static_assert(is_fixnum_hot(2)); // make_int(1)
    static_assert(tag_low2_hot(0) == 0);
    static_assert(tag_low2_hot(1) == 1);
    static_assert(tag_low2_hot(2) == 2);
    static_assert(tag_low2_hot(3) == 3);

    auto i0 = make_int(0);
    auto i1 = make_int(42);
    auto i_neg = make_int(-7);
    CHECK(is_int(i0) && is_fixnum_hot(i0.val), "is_int 0");
    CHECK(is_int(i1) && is_fixnum_hot(i1.val), "is_int 42");
    CHECK(is_int(i_neg) && is_fixnum_hot(i_neg.val), "is_int -7");
    CHECK(classify_eval_value_tag_consteval(i1.val) == EvalValueTag::Fixnum, "consteval Fixnum");
    CHECK(classify_eval_value_tag(i1.val) == EvalValueTag::Fixnum, "runtime Fixnum");

    auto s = make_string(0);
    CHECK(is_string(s) && is_string_v2_hot(s.val), "is_string pure");
    CHECK(!is_int(s), "string not int");
    CHECK(tag_low2_hot(s.val) == 3, "string low2 (#4482: 3)");

    // Ref: low2 == 1
    CHECK(is_ref_hot(1), "ref low2=1");
    CHECK(!is_fixnum_hot(1), "ref not fixnum");
    CHECK(!is_float_hot(i1.val), "int not float");
}

static void ac2_arithmetic_parity() {
    std::println("\n--- AC2: pure arithmetic via as_int (microbench proxy) ---");
    // Pure fixnum add loop — same results as baseline make_int/as_int path.
    std::int64_t acc = 0;
    bool all_int = true;
    for (int i = 0; i < 1000; ++i) {
        auto a = make_int(i);
        auto b = make_int(1);
        all_int = all_int && is_int(a) && is_int(b);
        acc += as_int(a) + as_int(b);
    }
    CHECK(all_int, "all operands classified as int");
    // sum_{i=0}^{999} (i+1) = sum_{k=1}^{1000} k = 1000*1001/2
    CHECK(acc == 1000 * 1001 / 2, "arithmetic parity sum");
}

static void ac3_contracts_present() {
    std::println("\n--- AC3: AURA_HOT_CONTRACT on as_* (source + happy path) ---");
    // Happy path: valid encoding — no abort under debug contracts.
    auto v = make_int(99);
    CHECK(as_int(v) == 99, "as_int contracted happy path");
    auto s = make_string(3);
    CHECK(is_string(s), "string ok");
    // Encoding invariants: fixnum low bit clear after make_int.
    CHECK((v.val & 1) == 0, "fixnum low bit clear");
    CHECK((s.val & 3) == 3, "string low2 == 3 (#4482)");
    // Wired flags prove contracts surface is present in production build.
    CHECK(value_tag_hotpath_2259_wired.load() == 1, "2259 wired");
    CHECK(value_tag_hotpath_zero_overhead_wired.load() == 1, "zero-overhead wired");
}

static void ac4_metrics() {
    std::println("\n--- AC4: value_tag_hot_path_total + schema-2259 ---");
    const auto hot0 = value_tag_hot_path_total.load(std::memory_order_relaxed);
    note_value_tag_hot_path();
    (void)as_int(make_int(1));
#ifdef AURA_CONTRACTS_OBSERVE
    // Soft / observe build: the process atomic is live (#2259).
    CHECK(value_tag_hot_path_total.load(std::memory_order_relaxed) > hot0, "hot path advanced");
#else
    // Issue #3391 (I1): under NDEBUG the fetch_add is compiled out —
    // the note is zero-overhead and the counter stays flat by contract.
    CHECK(value_tag_hot_path_total.load(std::memory_order_relaxed) == hot0,
          "hot path zero-overhead under NDEBUG (#3391: counter compiled out)");
#endif

    // Tag stability: consecutive Fixnum samples.
    const auto stab0 = value_tag_stability_run_total.load(std::memory_order_relaxed);
    const auto fx0 = value_tag_stability_fixnum_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 8; ++i) {
        note_value_tag_stability(EvalValueTag::Fixnum);
        (void)inline_shape_of(make_int(i).val);
    }
    CHECK(value_tag_stability_run_total.load(std::memory_order_relaxed) >= stab0,
          "stability runs monotonic");
    CHECK(value_tag_stability_fixnum_total.load(std::memory_order_relaxed) >= fx0,
          "fixnum stability monotonic");
    CHECK(inline_shape_of(make_int(5).val) == SHAPE_INT, "shape Fixnum");

    CompilerService cs;
    CHECK(href(cs, "schema-2259") == 2259, "schema-2259");
    CHECK(href(cs, "issue-2259") == 2259, "issue-2259");
    CHECK(href(cs, "value-tag-hotpath-zero-overhead-wired") == 1, "zero-overhead key");
    CHECK(href(cs, "value-tag-hotpath-2259-wired") == 1, "2259 wired key");
    CHECK(href(cs, "value-tag-hot-path-total") >= 0, "hot-path-total key");
    CHECK(href(cs, "value-tag-hot-contract-fail-total") >= 0, "contract-fail key");
    CHECK(href(cs, "value-tag-stability-fixnum-total") >= 0, "stability fixnum key");
    CHECK(href(cs, "schema") == 1622, "lineage schema 1622");
}

static void ac5_eval_semantics() {
    std::println("\n--- AC5: evaluator semantic parity ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (add1 x) (+ x 1)) (add1 41)\")").has_value(), "set-code");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42, "eval (+ x 1) => 42");
    auto a = cs.eval("(+ 10 20 30)");
    CHECK(a.has_value() && is_int(*a) && as_int(*a) == 60, "multi add");
    auto f = cs.eval("(* 6 7)");
    CHECK(f.has_value() && is_int(*f) && as_int(*f) == 42, "mul");
}

// ── Issue #4524: classify order + interpreter float-local slots ─────

static void ac6_classify_order_4524() {
    std::println("\n--- AC6 (#4524): fixnum-first single-branch hot tests match classify ---");
    // Static: the single-branch forms agree with the classifier at the
    // canonical encodings (the header carries more static_asserts).
    static_assert(is_fixnum_hot(0) && is_fixnum_hot(2) && is_fixnum_hot(-2));
    static_assert(!is_fixnum_hot(1) && !is_fixnum_hot(kSpecialFalse));
    static_assert(is_float_hot(types::make_float_raw_v2(0)) &&
                  is_float_hot(types::make_float_raw_v2(123456)));
    static_assert(!is_float_hot(types::make_string_raw_v2(7)) && !is_float_hot(0) &&
                  !is_float_hot(1) && !is_float_hot(kSpecialTrue));

    // Dynamic: full equivalence across every tag class.
    const std::vector<std::int64_t> samples = {
        0,
        2,
        -2,
        1 << 6,
        -(1 << 6),
        1,            // ref low2=01
        1 | (5 << 6), // ref Pair idx 5
        kSpecialFalse,
        kSpecialTrue,
        kSpecialVoid,
        kSpecialUnspecified,
        types::make_float_raw_v2(0),
        types::make_float_raw_v2(7),
        types::make_float_raw_v2(123456789),
        types::make_string_raw_v2(0),
        types::make_string_raw_v2(31),
        types::make_string_raw_v2(123456789),
    };
    for (const auto v : samples) {
        const auto cls = classify_eval_value_tag_consteval(v);
        CHECK(is_fixnum_hot(v) == (cls == EvalValueTag::Fixnum), "fixnum-hot ≡ classify");
        CHECK(is_float_hot(v) == (cls == EvalValueTag::Float), "float-hot ≡ classify");
    }

    // Roundtrips through the real constructors classify identically.
    for (int i = 0; i < 64; ++i) {
        const auto iv = make_int(i);
        CHECK(is_int(iv) && is_fixnum_hot(iv.val), "make_int fixnum-hot");
        CHECK(!is_float(iv), "int not float");
    }
    auto fv = make_float(2.5);
    CHECK(is_float(fv) && is_float_hot(fv.val), "make_float float-hot");
    CHECK(!is_int(fv), "float not int");
    CHECK(as_float(fv) == 2.5, "float roundtrip");
}

// Float-pool traffic probe: allocs attributable to `body` = idx delta - 1
// (the pre-probe itself occupies one slot).
static std::int64_t float_pool_probe_pre(double probe) {
    return types::float_idx_raw_v2(aura_alloc_float(probe));
}
static std::int64_t float_pool_allocs_since(double probe, std::int64_t pre) {
    return types::float_idx_raw_v2(aura_alloc_float(probe)) - pre - 1;
}

static void ac7_float_local_loop_4524() {
    std::println("\n--- AC7 (#4524): interpreter float micro-loop alloc churn ---");
    CompilerService cs;
    // Top-level named-let expressions run the eval_flat interpreter TCO
    // backedge (service eval -> evaluator_.eval_flat) - the path this
    // issue optimizes.
    const char* floop = "(let lp ((acc 0.0) (i 0)) (if (< i 1000) (lp (+ acc 1.0) (+ i 1)) acc))";
    const char* floop_big =
        "(let lp ((acc 0.0) (i 0)) (if (< i 200000) (lp (+ acc 1.0) (+ i 1)) acc))";
    const char* floop_var =
        "(let lp ((acc 0.0) (i 0)) (if (< i 1000) (lp (+ acc 1.1) (+ i 1)) acc))";
    const char* sloop = "(let lp ((acc 10.0) (i 0)) (if (< i 4) (lp (- acc 1.5) (+ i 1)) acc))";
    const char* ploop = "(let lp ((acc 1.1) (i 0)) (if (< i 3) (lp (* acc 2.0) (+ i 1)) acc))";
    const char* iloop = "(let lp ((acc 0) (i 0)) (if (< i 100) (lp (+ acc 1) (+ i 1)) acc))";
    // Escape rebox: mid-loop cons snapshots of acc must keep the value the
    // boxed path froze - the boxes are fresh and immutable per iteration.
    const char* eloop = "(let lp ((acc 0.0) (i 0) (snap '()))"
                        " (if (< i 5) (lp (+ acc 1.0) (+ i 1) (cons acc snap))"
                        "    (let sum ((l snap) (t 0.0))"
                        "      (if (null? l) t (sum (cdr l) (+ t (car l)))))))";

    // AC7.1 (#5302): the named-let C++ loop keeps the float accumulator raw.
    // The boxed baseline allocated a pool slot for the step every iteration
    // (~2N). Entry literals, the first interpreted step, and the result are
    // a handful of boxes. Per-step boxing would be ~1000 here.
    {
        const auto pre = float_pool_probe_pre(1234.5);
        auto r = cs.eval(floop);
        const auto allocs = float_pool_allocs_since(1234.5, pre);
        std::println("    floop 1000: aura_alloc_float count = {}", allocs);
        CHECK(r.has_value() && is_float(*r) && as_float(*r) == 1000.0, "floop sum 1000.0");
        CHECK(allocs < 48, "named-let float loop boxes O(1), not one per step");
    }

    // AC7.2 results stay exact across + - * and mixed promotion.
    {
        auto s = cs.eval(sloop);
        CHECK(s.has_value() && is_float(*s) && as_float(*s) == 4.0, "sub loop 10.0-4*1.5");
        auto p = cs.eval(ploop);
        const double expect = (1.1 * 2.0) * 2.0 * 2.0;
        CHECK(p.has_value() && is_float(*p) && as_float(*p) == expect, "mul loop parity");
        auto m = cs.eval(floop_var);
        double mv = 0.0;
        for (int i = 0; i < 1000; ++i)
            mv = 0.0 + mv + 1.1;
        CHECK(m.has_value() && is_float(*m) && as_float(*m) == mv, "variable-step parity");
    }

    // AC7.3 pure-fixnum loop: no float pool traffic at all.
    {
        const auto pre = float_pool_probe_pre(7.5);
        auto r = cs.eval(iloop);
        const auto allocs = float_pool_allocs_since(7.5, pre);
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 100, "int loop sum");
        CHECK(allocs == 0, "fixnum loop allocates zero float boxes");
    }

    // AC7.4 escape rebox: snapshot cons cells hold pre-increment acc
    // values 0..4; an in-place-mutated step box would come out 20.0
    // (5 x final) instead of 10.0.
    {
        auto e = cs.eval(eloop);
        CHECK(e.has_value() && is_float(*e) && as_float(*e) == 10.0,
              "escape snapshot values frozen (0+1+2+3+4)");
    }

    // AC7.5 generic + - * semantics unchanged where the fast path is not
    // taken: overflow errors, non-numbers reject, promotions stable.
    {
        auto of = cs.eval("(+ 4611686018427387903 4611686018427387903)");
        CHECK(of.has_value() && is_error(*of), "fixnum overflow still errors");
        auto tn = cs.eval("(+ 1.0 2)");
        CHECK(tn.has_value() && is_float(*tn) && as_float(*tn) == 3.0, "int+float promote");
        auto bad = cs.eval("(+ 1.0 \"s\")");
        CHECK(bad.has_value() && is_error(*bad), "non-number still errors");
        auto e1 = cs.eval("(eq? 1.0 1.0)");
        CHECK(e1.has_value(), "float literal eq? evaluable");
    }

    // AC7.6 steady-state timing (indicative; counts above are the metric).
    {
        (void)cs.eval(floop); // warm intern caches
        double best_ns = 1e18;
        for (int rep = 0; rep < 3; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            auto r = cs.eval(floop_big);
            const auto t1 = std::chrono::steady_clock::now();
            CHECK(r.has_value() && is_float(*r) && as_float(*r) == 200000.0, "big loop sum");
            const double ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 200000.0;
            best_ns = ns < best_ns ? ns : best_ns;
        }
        std::println("    floop steady-state: {:.1f} ns/iter (best of 3)", best_ns);
    }
}

// #5304: a named-let whose step calls a primitive or a literal function stays
// on the integer loop after the 16-site table is full, and a side-effecting
// call still runs once per step.
static void ac8_named_let_calls_5304() {
    std::println("\n--- AC8 (#5304): named-let calls stay on the integer loop ---");
    CompilerService cs;
    std::int64_t mod20 = 0;
    for (int i = 0; i < 20; ++i)
        mod20 += i % 7;
    std::int64_t mod_big = 0;
    constexpr int kN = 200000;
    for (int i = 0; i < kN; ++i)
        mod_big += i % 7;

    std::string fillers;
    for (int i = 0; i < 12; ++i) {
        fillers += "(define (fill" + std::to_string(i) +
                   ") (let loop ((i 0) (s 0)) (if (< i 3) (loop (+ i 1) (+ s " + std::to_string(i) +
                   ")) s)))\n(fill" + std::to_string(i) + ")\n";
    }
    const std::string src =
        "(define V (vector 1 2 3 4 5 6 7 8))\n"
        "(define (five) 5)\n"
        "(define T 0)\n"
        "(define (tick) (set! T (+ T 1)) 1)\n"
        "(define (a n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s 3)) s)))\n"
        "(define (b n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (five))) s)))\n"
        "(define (c n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (vector-ref V 5))) "
        "s)))\n"
        "(define (d n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (modulo i 7))) s)))\n"
        "(define (dm n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (mod i 7))) s)))\n"
        "(define (e n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (list-ref (list 1 2 "
        "3) 2))) s)))\n"
        "(define (f n) (let ((v (vector 1 2 3 4 5 6 7 8))) (let loop ((i 0) (s 0)) (if (< i n) "
        "(loop (+ i 1) (+ s (vector-ref v 5))) s))))\n"
        "(define (g n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (tick))) s)))\n"
        "(define sa (a 20))\n"
        "(define sb (b 20))\n"
        "(define sc (c 20))\n"
        "(define sd (d 20))\n"
        "(define se (e 20))\n"
        "(define sf (f 20))\n"
        "(define sg (g 20))\n"
        "(define st T)\n"
        "(vector-set! V 5 9)\n"
        "(define sc2 (c 4))\n" +
        fillers +
        "(define t0 (monotonic-ms))\n"
        "(define rc (a " +
        std::to_string(kN) +
        "))\n"
        "(define t1 (monotonic-ms))\n"
        "(define rm (d " +
        std::to_string(kN) +
        "))\n"
        "(define t2 (monotonic-ms))\n"
        "(display (list sa sb sc sd se sf sg st sc2 rc rm (- t1 t0) (- t2 t1)))\n"
        "(newline)\n"
        "(if (and (= sa 60) (= sb 100) (= sc 120) (= sd " +
        std::to_string(mod20) +
        ") (= se 60) (= sf 120) (= sg 20) (= st 20) (= sc2 36)"
        " (= rc " +
        std::to_string(static_cast<long long>(3) * kN) + ") (= rm " + std::to_string(mod_big) +
        ") (= (dm 20) sd)"
        " (< (- t2 t1) (+ (* (- t1 t0) 3) 15))) 1 0)\n";
    auto g = cs.eval(src);
    CHECK(g.has_value() && types::is_int(*g) && types::as_int(*g) == 1,
          "5304 values match and modulo stays within 3x of the const loop");

    // A changing index, vector-length, and car stay on the integer loop.
    // The broken path was ~13x the plain add. Length and car are one copy.
    constexpr int kSteps = 200000;
    std::int64_t mod_sum = 0;
    for (int i = 0; i < kSteps; ++i)
        mod_sum += (i % 8) + 1;
    const std::string live =
        "(define Vt (vector 1 2 3 4 5 6 7 8))\n"
        "(define Vu (vector 1 2 3 4 5 6 7 8))\n"
        "(define (sum-i v n) (let loop ((i 0) (s 0)) (if (>= i n) s (loop (+ i 1) (+ s (vector-ref "
        "v i))))))\n"
        "(define (sum-off v n) (let loop ((i 1) (s 0)) (if (> i n) s (loop (+ i 1) (+ s "
        "(vector-ref "
        "v (- i 1)))))))\n"
        "(define (sum-len v n) (let loop ((i 0) (s 0)) (if (>= i n) s (loop (+ i 1) (+ s "
        "(vector-length v))))))\n"
        "(define (sum-car p n) (let loop ((i 0) (s 0)) (if (>= i n) s (loop (+ i 1) (+ s (car "
        "p))))))\n"
        "(define (sum-mod n) (let loop ((i 0) (s 0)) (if (>= i n) s (loop (+ i 1) (+ s (vector-ref "
        "Vu (modulo i 8)))))))\n"
        "(define (sum-plain n) (let loop ((i 0) (s 0)) (if (>= i n) s (loop (+ i 1) (+ s i)))))\n"
        "(define (sum-sh v n) (let ((vector-ref (lambda (a b) 1))) (let loop ((i 0) (s 0)) (if (>= "
        "i n) s (loop (+ i 1) (+ s (vector-ref v i)))))))\n"
        "(define (sum-f v n) (let loop ((i 0) (s 0.0)) (if (>= i n) s (loop (+ i 1) (+ s "
        "(vector-ref v i))))))\n"
        "(define p (cons 6 0))\n"
        "(define a (sum-i (vector 1 2 3 4) 4))\n"
        "(define b (sum-off (vector 1 2 3 4) 4))\n"
        "(define c (sum-len Vt 5))\n"
        "(define d (sum-car p 5))\n"
        "(set-car! p 8)\n"
        "(define d2 (sum-car p 5))\n"
        "(define e (sum-sh (vector 9 9 9 9) 4))\n"
        "(define f (sum-f (vector 1.5 2.5) 2))\n"
        "(vector-set! Vt 1 10)\n"
        "(define a2 (sum-i Vt 4))\n"
        "(define t0 (monotonic-ms))\n"
        "(define rp (sum-plain " +
        std::to_string(kSteps) +
        "))\n"
        "(define t1 (monotonic-ms))\n"
        "(define rm (sum-mod " +
        std::to_string(kSteps) +
        "))\n"
        "(define t2 (monotonic-ms))\n"
        "(define rl (sum-len Vt " +
        std::to_string(kSteps) +
        "))\n"
        "(define t3 (monotonic-ms))\n"
        "(define rc (sum-car (cons 1 2) " +
        std::to_string(kSteps) +
        "))\n"
        "(define t4 (monotonic-ms))\n"
        "(display (list a b c d d2 e f a2 rp rm rl rc (- t1 t0) (- t2 t1) (- t3 t2) (- t4 t3)))\n"
        "(newline)\n"
        "(if (and (= a 10) (= b 10) (= c 40) (= d 30) (= d2 40) (= e 4) (= f 4.0) (= a2 18)"
        " (= rp " +
        std::to_string(static_cast<long long>(kSteps) * (kSteps - 1) / 2) + ") (= rm " +
        std::to_string(mod_sum) + ") (= rl " + std::to_string(static_cast<long long>(8) * kSteps) +
        ") (= rc " + std::to_string(kSteps) +
        ")"
        " (< (- t2 t1) (+ (* (- t1 t0) 6) 40))"
        " (< (- t3 t2) (+ (* (- t1 t0) 3) 20))"
        " (< (- t4 t3) (+ (* (- t1 t0) 3) 20))) 1 0)\n";
    auto live_r = cs.eval(live);
    CHECK(live_r.has_value() && types::is_int(*live_r) && types::as_int(*live_r) == 1,
          "5304 variable vector-ref, length, and car match and stay near the add loop");
    auto oob =
        cs.eval("(let ((v (vector 1))) (let loop ((i 0) (s 0)) (if (>= i 3) s (loop (+ i 1) (+ s "
                "(vector-ref v i))))))");
    CHECK(oob.has_value() && is_error(*oob), "5304 vector-ref past the end still errors");
    auto neg = cs.eval(
        "(let ((v (vector 1 2 3))) (let loop ((i 0) (s 0)) (if (>= i 2) s (loop (+ i 1) (+ s "
        "(vector-ref v (- i 1)))))))");
    CHECK(neg.has_value() && is_error(*neg), "5304 negative vector-ref still errors");
}

// #5307: hash-ref of a literal key in a named let is one copy. A vector
// value with a varying index re-reads that slot. A missing key still errors.
static void ac9_named_let_hash_ref_5307() {
    std::println("\n--- AC9 (#5307): named-let hash-ref stays on the numeric loop ---");
    CompilerService cs;
    constexpr int kSteps = 200000;
    double mod_sum = 0.0;
    for (int i = 0; i < kSteps; ++i)
        mod_sum += static_cast<double>((i % 4) + 1);
    const std::string live =
        "(define Hv (hash \"px\" (vector 1.0 2.0 3.0 4.0 5.0 6.0) \"n\" 3 \"a\" 1 \"b\" 2 \"c\" "
        "3))\n"
        "(define (sum-v n) (let loop ((i 0) (s 0.0)) (if (< i n) (loop (+ i 1) (+ s (vector-ref "
        "(hash-ref Hv \"px\") 5))) s)))\n"
        "(define (sum-var n) (let loop ((i 0) (s 0.0)) (if (< i n) (loop (+ i 1) (+ s (vector-ref "
        "(hash-ref Hv \"px\") (modulo i 4)))) s)))\n"
        "(define (sum-off n) (let loop ((i 1) (s 0.0)) (if (> i n) s (loop (+ i 1) (+ s "
        "(vector-ref (hash-ref Hv \"px\") (- i 1)))))))\n"
        "(define (sum-i n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (hash-ref Hv "
        "\"n\"))) s)))\n"
        "(define (sum-3 n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ (+ (+ s (hash-ref "
        "Hv \"a\")) (hash-ref Hv \"b\")) (hash-ref Hv \"c\"))) s)))\n"
        "(define HK (hash 2 8))\n"
        "(define (sum-k n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (hash-ref HK "
        "2))) s)))\n"
        "(define (sum-arg t n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (hash-ref t "
        "\"n\"))) s)))\n"
        "(define (sum-sh n) (let ((hash-ref (lambda (h k) 1))) (let loop ((i 0) (s 0)) (if (< i n) "
        "(loop (+ i 1) (+ s (hash-ref Hv \"n\"))) s))))\n"
        "(define (sum-re) (let loop ((i 0) (s 0) (h (hash \"n\" 4))) (if (>= i 2) s (loop (+ i 1) "
        "(+ s (hash-ref h \"n\")) (hash \"n\" 9)))))\n"
        "(define (sum-def n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (hash-ref Hv "
        "\"missing\" 7))) s)))\n"
        "(define (sum-plain n) (let loop ((i 0) (s 0.0)) (if (< i n) (loop (+ i 1) (+ s 1.0)) "
        "s)))\n"
        "(define HN (hash \"n\" 3))\n"
        "(define (sum-n n) (let loop ((i 0) (s 0)) (if (< i n) (loop (+ i 1) (+ s (hash-ref HN "
        "\"n\"))) s)))\n"
        "(define a (sum-v 4))\n"
        "(define b (sum-var 4))\n"
        "(define c (sum-off 4))\n"
        "(define d (sum-i 4))\n"
        "(define e (sum-3 4))\n"
        "(define f (sum-k 4))\n"
        "(define g (sum-arg (hash \"n\" 6) 3))\n"
        "(define g2 (sum-arg (hash \"n\" 2) 3))\n"
        "(define h (sum-sh 4))\n"
        "(define rb (sum-re))\n"
        "(define df (sum-def 4))\n"
        "(define n1 (sum-n 4))\n"
        "(hash-set! HN \"n\" 5)\n"
        "(define n2 (sum-n 4))\n"
        "(define Vx (vector 1.0 2.0 3.0 4.0 5.0 6.0))\n"
        "(define Hx (hash \"px\" Vx))\n"
        "(define (sum-x n) (let loop ((i 0) (s 0.0)) (if (< i n) (loop (+ i 1) (+ s (vector-ref "
        "(hash-ref Hx \"px\") i))) s)))\n"
        "(define x1 (sum-x 4))\n"
        "(vector-set! Vx 1 10.0)\n"
        "(define x2 (sum-x 4))\n"
        "(define t0 (monotonic-ms))\n"
        "(define rp (sum-plain " +
        std::to_string(kSteps) +
        "))\n"
        "(define t1 (monotonic-ms))\n"
        "(define rv (sum-v " +
        std::to_string(kSteps) +
        "))\n"
        "(define t2 (monotonic-ms))\n"
        "(define rm (sum-var " +
        std::to_string(kSteps) +
        "))\n"
        "(define t3 (monotonic-ms))\n"
        "(define ri (sum-i " +
        std::to_string(kSteps) +
        "))\n"
        "(define t4 (monotonic-ms))\n"
        "(display (list a b c d e f g g2 h rb df n1 n2 x1 x2 rp rv rm ri (- t1 t0) (- t2 t1) (- t3 "
        "t2) (- t4 t3)))\n"
        "(newline)\n"
        "(if (and (= a 24.0) (= b 10.0) (= c 10.0) (= d 12) (= e 24) (= f 32) (= g 18) (= g2 6)"
        " (= h 4) (= rb 13) (= df 28) (= n1 12) (= n2 20) (= x1 10.0) (= x2 18.0)"
        " (= rp " +
        std::to_string(kSteps) + ".0) (= rv " + std::to_string(static_cast<long long>(6) * kSteps) +
        ".0) (= rm " + std::to_string(static_cast<long long>(mod_sum)) + ".0) (= ri " +
        std::to_string(static_cast<long long>(3) * kSteps) +
        ")"
        " (< (- t2 t1) (+ (* (- t1 t0) 3) 40))"
        " (< (- t3 t2) (+ (* (- t1 t0) 3) 40))"
        " (< (- t4 t3) (+ (* (- t1 t0) 3) 40))) 1 0)\n";
    auto live_r = cs.eval(live);
    CHECK(live_r.has_value() && types::is_int(*live_r) && types::as_int(*live_r) == 1,
          "5307 hash-ref values match and stay near the add loop");
    auto miss = cs.eval(
        "(let ((h (hash \"n\" 1))) (let loop ((i 0) (s 0)) (if (>= i 2) s (loop (+ i 1) (+ s "
        "(hash-ref h \"no\"))))))");
    CHECK(miss.has_value() && is_error(*miss), "5307 missing hash key still errors");
    auto oob = cs.eval(
        "(let ((h (hash \"px\" (vector 1)))) (let loop ((i 0) (s 0)) (if (>= i 3) s (loop (+ i 1) "
        "(+ s (vector-ref (hash-ref h \"px\") i))))))");
    CHECK(oob.has_value() && is_error(*oob), "5307 vector-ref past the end still errors");
}

} // namespace

int run_test_value_tag_hot_path() {
    std::println("=== Issue #2259: Value tag hot-path contracts + zero-overhead dispatch ===");
    ac1_pure_tag_tests();
    ac2_arithmetic_parity();
    ac3_contracts_present();
    ac4_metrics();
    ac5_eval_semantics();
    ac6_classify_order_4524();
    ac7_float_local_loop_4524();
    ac8_named_let_calls_5304();
    ac9_named_let_hash_ref_5307();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_value_tag_hot_path();
}
#endif
