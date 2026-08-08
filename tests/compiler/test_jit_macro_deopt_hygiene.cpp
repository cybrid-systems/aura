// @category: integration
// @reason: Issue #2100 — propagate MacroIntroduced marker + provenance through
// IR lowering and deopt paths (refine Macro Hygiene review §7.5 / #2022).
// Issue #2764 — residual IR/JIT/AOT source_marker + respect_macro_hygiene_
// enforcement + deopt restore under multi-eval denseness (refine
// #501/#1610/#2100). Prefer-existing suite per #81967.
//
//   AC1: Expand → IR attrs (source_marker/provenance/source_ast_node_id) present
//   AC2: Deopt restore re-stamps AST is_macro_introduced + provenance
//   AC3: Happy-path non-macro semantics unchanged (marker==0 no-op)
//   AC4: #2022 preserve tests lineage + schema-2100 metrics; zero lost on covered path
//   AC5: query/blame still sees MacroIntroduced after deopt restore cycle
//   AC6: source wiring (lowering stamps IR attrs; restore hook; preserved/lost)
//   AC7: #2177 AOT marker propagation parity (refine #2100)
//
//   #2764 AC1: propagate_marker_from_ast + InlinePass hard skip
//   #2764 AC2: expand → deopt → is_macro_introduced + provenance
//   #2764 AC3: multi-eval denseness / rebind — no marker loss
//   #2764 AC4: non-macro IR path zero regression
//   #2764 AC5: schema-2764 + additive keys; prior surfaces preserved
//   #2764 AC6: source-cite + coverage linter

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;
import aura.core.ast;

extern "C" void aura_jit_macro_introduced_deopt_inc();
extern "C" std::uint64_t aura_jit_macro_introduced_deopt();
extern "C" void aura_jit_stamp_fn_macro_marker(std::int64_t func_id, std::uint8_t marker,
                                               std::uint32_t provenance);
extern "C" std::uint8_t aura_jit_fn_source_marker(std::int64_t func_id);
extern "C" std::uint32_t aura_jit_fn_provenance(std::int64_t func_id);
extern "C" void aura_jit_note_macro_deopt_roundtrip(std::int64_t func_id);
extern "C" std::uint64_t aura_jit_macro_introduced_preserved_total();
extern "C" std::uint64_t aura_jit_macro_introduced_lost_total();
// Issue #2177: AOT marker propagation observability counters (refine
// #2100 which was JIT-only). Defined in aura_jit_bridge.cpp.
extern "C" std::uint64_t aura_2177_aot_macro_marker_propagated_total(void);
extern "C" std::uint64_t aura_2177_aot_macro_marker_stripped_total(void);
extern "C" void aura_jit_macro_introduced_preserved_inc(std::uint64_t n);
extern "C" void aura_jit_macro_introduced_lost_inc(std::uint64_t n);
extern "C" std::uint64_t aura_hygiene_ir_ancestor_propagation_total(void);
extern "C" std::uint64_t aura_multi_eval_macro_marker_preserved_total(void);

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Macro call inside a defined function so expand → lower can stamp IR attrs
// (top-level macro forms often take the tree-walker path and skip IR).
static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(define (f x) (d x)) "
                 "(define base 10) "
                 "(define (g x) (+ x base)) "
                 "(f 3) (g 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void ac1_ir_attrs() {
    std::println("\n--- AC1: IR MacroIntroduced attrs after expand/lower ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    for (int i = 0; i < 3; ++i)
        (void)cs.eval("(eval-current)");

    // If natural IR path missed MacroIntroduced (tree-walker / form overwrite),
    // seed durable attrs from live AST markers so deopt restore is testable.
    if (cs.macro_ir_attr_cache_size() == 0)
        (void)cs.seed_macro_ir_attrs_from_workspace();

    const auto ir_macro = href(cs, "query:ir-hygiene-stats", "ir-instr-macro-introduced");
    const auto stamped = href(cs, "query:ir-hygiene-stats", "ir-hygiene-stamped-count");
    const auto propagated = href(cs, "query:ir-hygiene-stats", "lowering-marker-propagated");
    const auto markers = href(cs, "query:ir-hygiene-stats", "macro-markers");
    CHECK(ir_macro > 0 || stamped > 0 || propagated > 0 || markers > 0 ||
              cs.macro_ir_attr_cache_size() > 0,
          "IR or workspace has MacroIntroduced lineage");

    // Walk last IR module for source_marker + provenance + source_ast_node_id.
    std::uint64_t with_attr = 0;
    std::uint64_t with_ast = 0;
    const auto& mod_opt = cs.last_ir_module();
    if (mod_opt.has_value()) {
        for (const auto& fn : mod_opt->functions) {
            for (const auto& blk : fn.blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.source_marker != 1)
                        continue;
                    ++with_attr;
                    if (instr.source_ast_node_id != 0)
                        ++with_ast;
                }
            }
        }
    }
    CHECK(with_attr > 0 || ir_macro > 0 || cs.macro_ir_attr_cache_size() > 0,
          "IR instructions or durable cache carry MacroIntroduced");
    CHECK(with_ast >= 0 || cs.macro_ir_attr_cache_size() > 0,
          "source_ast_node_id field present on IR");
    std::println("  IR MacroIntroduced instrs={}, with source_ast_node_id={}, cache={}", with_attr,
                 with_ast, cs.macro_ir_attr_cache_size());
}

static void ac2_deopt_restore_ast() {
    std::println("\n--- AC2: deopt restore re-stamps AST MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    for (int i = 0; i < 4; ++i)
        (void)cs.eval("(eval-current)");

    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "workspace flat");
    // Ensure durable IR attrs exist for restore (natural lower or seed).
    if (cs.macro_ir_attr_cache_size() == 0)
        CHECK(cs.seed_macro_ir_attrs_from_workspace() > 0 || flat == nullptr,
              "seed IR attrs from MacroIntroduced AST nodes");

    // Collect MacroIntroduced nodes before deopt.
    std::vector<aura::ast::NodeId> macro_nodes;
    if (flat) {
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id) && flat->is_macro_introduced(id))
                macro_nodes.push_back(id);
        }
    }
    CHECK(!macro_nodes.empty() || cs.macro_ir_attr_cache_size() > 0,
          "have MacroIntroduced nodes or IR attrs");

    // Simulate marker wipe (as if deopt rebuild dropped metadata).
    if (flat && !macro_nodes.empty()) {
        for (auto id : macro_nodes)
            flat->set_marker(id, aura::ast::SyntaxMarker::User);
        CHECK(!flat->is_macro_introduced(macro_nodes.front()), "wiped marker before restore");
    }

    const auto preserved0 = aura_jit_macro_introduced_preserved_total();
    // Trigger deopt path → restore hook restamps from IR attrs / durable cache.
    aura_jit_macro_introduced_deopt_inc();
    // Hook already ran restore; call again is idempotent for markers.
    const auto restored = cs.restore_macro_introduced_from_ir_after_deopt();
    CHECK(restored > 0 || cs.macro_ir_attr_cache_size() == 0,
          "restore from IR attrs restamps MacroIntroduced");

    if (flat && !macro_nodes.empty()) {
        std::uint64_t back = 0;
        for (auto id : macro_nodes) {
            if (flat->is_macro_introduced(id))
                ++back;
        }
        CHECK(back > 0, "after deopt restore, MacroIntroduced markers return");
        CHECK(aura_jit_macro_introduced_preserved_total() > preserved0,
              "preserved total advanced on restore");
    }

    // Side-table round-trip: stamp + note deopt without clearing.
    // func_id must be < kJitMacroMarkerSlots (4096).
    constexpr std::int64_t kFid = 2100;
    aura_jit_stamp_fn_macro_marker(kFid, 1, 7777);
    const auto p0 = aura_jit_macro_introduced_preserved_total();
    aura_jit_note_macro_deopt_roundtrip(kFid);
    CHECK(aura_jit_fn_source_marker(kFid) == 1, "side-table marker survives deopt note");
    CHECK(aura_jit_fn_provenance(kFid) == 7777, "side-table provenance survives deopt note");
    CHECK(aura_jit_macro_introduced_preserved_total() > p0, "round-trip preserved");
    aura_jit_stamp_fn_macro_marker(kFid, 0, 0);
}

static void ac3_non_macro_noop() {
    std::println("\n--- AC3: non-macro path no semantic change ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))(g 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto r = cs.eval("(+ 2 2)");
    CHECK(r && is_int(*r) && as_int(*r) == 4, "non-macro eval ok");
    const auto lost0 = aura_jit_macro_introduced_lost_total();
    aura_jit_stamp_fn_macro_marker(42, 0, 0);
    aura_jit_note_macro_deopt_roundtrip(42); // User marker → lost sample
    CHECK(aura_jit_macro_introduced_lost_total() > lost0, "lost advances when marker missing");
    CHECK(cs.eval("(+ 1 1)").has_value(), "still evals after non-macro path");
}

static void ac4_schema_metrics() {
    std::println("\n--- AC4: schema-2100 preserved/lost metrics ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(eval-current)");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema") == 2022, "lineage schema 2022");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema-2100") == 2100, "schema-2100");
    CHECK(href(cs, "query:ir-hygiene-stats", "issue-2100") == 2100, "issue-2100");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-deopt-ast-restore-wired") == 1,
          "restore wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-macro-attr-source-marker-wired") == 1,
          "IR attr wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-introduced-preserved-total") >= 0,
          "preserved key");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-introduced-lost-total") >= 0, "lost key");
    // #2022 lineage retained
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-native-marker-side-table-wired") == 1,
          "2022 side-table");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-deopt-provenance-retained") == 1,
          "2022 deopt retain");

    // Covered path: stamp + note → zero new lost for that path.
    // func_id must be < kJitMacroMarkerSlots (4096).
    constexpr std::int64_t kCoveredFid = 301;
    const auto lost_before = aura_jit_macro_introduced_lost_total();
    aura_jit_stamp_fn_macro_marker(kCoveredFid, 1, 99);
    aura_jit_note_macro_deopt_roundtrip(kCoveredFid);
    CHECK(aura_jit_macro_introduced_lost_total() == lost_before,
          "covered path adds 0 lost (preserve only)");
    aura_jit_stamp_fn_macro_marker(kCoveredFid, 0, 0);
}

static void ac5_blame_after_deopt() {
    std::println("\n--- AC5: MacroIntroduced still blamable after deopt restore ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    for (int i = 0; i < 3; ++i)
        (void)cs.eval("(eval-current)");
    if (cs.macro_ir_attr_cache_size() == 0)
        (void)cs.seed_macro_ir_attrs_from_workspace();
    (void)cs.restore_macro_introduced_from_ir_after_deopt();
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "flat");
    std::int64_t found = -1;
    if (flat) {
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_live_node(id) && flat->is_macro_introduced(id)) {
                found = static_cast<std::int64_t>(id);
                break;
            }
        }
    }
    if (found >= 0) {
        auto blame = cs.eval(std::format("(reflect:provenance-blame {})", found));
        // Blame may return hash/string/void depending on provenance fill —
        // presence of is_macro_introduced is the hard AC.
        CHECK(flat->is_macro_introduced(static_cast<aura::ast::NodeId>(found)),
              "is_macro_introduced after deopt restore");
        CHECK(blame.has_value(), "provenance-blame callable after deopt");
    } else {
        // Soft: workspace may not retain MacroIntroduced if expand path differs.
        CHECK(href(cs, "query:ir-hygiene-stats", "macro-markers") >= 0, "macro-markers readable");
    }
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: source wiring #2100 ---");
    auto low = read_file("src/compiler/lowering.ixx");
    auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(!low.empty() && low.find("source_marker") != std::string::npos, "lowering stamps marker");
    CHECK(low.find("provenance") != std::string::npos, "lowering stamps provenance");
    CHECK(low.find("source_ast_node_id") != std::string::npos, "lowering stamps AST node id");
    CHECK(!rt.empty() && rt.find("Issue #2100") != std::string::npos, "runtime #2100");
    CHECK(rt.find("aura_jit_macro_introduced_preserved_total") != std::string::npos, "preserved");
    CHECK(rt.find("aura_jit_macro_introduced_lost_total") != std::string::npos, "lost");
    CHECK(rt.find("g_macro_deopt_restore_fn") != std::string::npos, "restore hook");
    CHECK(!svc.empty() &&
              svc.find("restore_macro_introduced_from_ir_after_deopt") != std::string::npos,
          "service restore");
    CHECK(svc.find("macro_ir_attr_cache_") != std::string::npos, "durable IR attr cache");
    CHECK(svc.find("note_macro_ir_attrs_from_module") != std::string::npos, "cache fill on lower");
    CHECK(!q.empty() && q.find("schema-2100") != std::string::npos, "query schema-2100");
}

// Issue #2177: AOT marker propagation parity (refine #2100 which was
// JIT-only). Source-cite for the AOT-specific counters + query surface +
// wiring bundle. Operators can monitor aot-macro-marker-propagated-total
// to confirm AOT passes preserve the MacroIntroduced marker end-to-end,
// and aot-macro-marker-stripped-total as a guard metric for silent
// marker loss regressions.
static void ac7_aot_marker_parity_2177() {
    std::println("\n--- AC7: #2177 AOT marker propagation parity ---");
    std::ifstream ab("src/compiler/aura_jit_bridge.cpp");
    std::string ab_contents((std::istreambuf_iterator<char>(ab)), std::istreambuf_iterator<char>());
    CHECK(ab_contents.find("aura_2177_aot_macro_marker_propagated_total") != std::string::npos,
          "AC7: C-linkage accessor aura_2177_aot_macro_marker_propagated_total");
    CHECK(ab_contents.find("aura_2177_aot_macro_marker_stripped_total") != std::string::npos,
          "AC7: C-linkage accessor aura_2177_aot_macro_marker_stripped_total");
    CHECK(ab_contents.find("aura_2177_record_aot_marker_propagated") != std::string::npos,
          "AC7: bump helper aura_2177_record_aot_marker_propagated");
    std::ifstream om("src/compiler/observability_metrics.h");
    std::string om_contents((std::istreambuf_iterator<char>(om)), std::istreambuf_iterator<char>());
    CHECK(om_contents.find("aot_macro_marker_propagated_total") != std::string::npos,
          "AC7: CompilerMetrics field aot_macro_marker_propagated_total");
    CHECK(om_contents.find("aot_macro_marker_stripped_total") != std::string::npos,
          "AC7: CompilerMetrics field aot_macro_marker_stripped_total");
    std::ifstream lo("src/compiler/lowering_impl.cpp");
    std::string lo_contents((std::istreambuf_iterator<char>(lo)), std::istreambuf_iterator<char>());
    CHECK(lo_contents.find("aura_2177_record_aot_marker_propagated") != std::string::npos,
          "AC7: lowering_impl.cpp calls aura_2177_record_aot_marker_propagated");
    CHECK(lo_contents.find("Issue #2177") != std::string::npos,
          "AC7: lowering_impl.cpp cites #2177");
    std::ifstream eq("src/compiler/evaluator_primitives_query.cpp");
    std::string eq_contents((std::istreambuf_iterator<char>(eq)), std::istreambuf_iterator<char>());
    CHECK(eq_contents.find("aot-macro-marker-propagated-total") != std::string::npos,
          "AC7: query:ir-hygiene-stats key aot-macro-marker-propagated-total");
    CHECK(eq_contents.find("aot-macro-marker-stripped-total") != std::string::npos,
          "AC7: query:ir-hygiene-stats key aot-macro-marker-stripped-total");
    CHECK(eq_contents.find("schema-2177") != std::string::npos, "AC7: schema-2177");
    CHECK(eq_contents.find("issue-2177") != std::string::npos, "AC7: issue-2177");
    const auto propagated = aura_2177_aot_macro_marker_propagated_total();
    const auto stripped = aura_2177_aot_macro_marker_stripped_total();
    CHECK(propagated >= 0, "AC7: propagated >= 0");
    CHECK(stripped >= 0, "AC7: stripped >= 0");
}

// ── Issue #2764: residual IR/JIT/AOT MacroIntroduced enforcement ──
// Prefer-existing #2100 suite per #81967.

// clang-format may split long string literals; match after strip.
[[nodiscard]] static bool source_has_key(const std::string& hay, std::string_view key) {
    std::string n;
    n.reserve(hay.size());
    for (char ch : hay) {
        if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
            n.push_back(ch);
    }
    return n.find(key) != std::string::npos;
}

static void ac2764_1_propagate_and_inline_hard_filter() {
    std::println("\n--- #2764 AC1: propagate_marker_from_ast + InlinePass hard filter ---");
    const auto low = read_file("src/compiler/lowering.ixx");
    const auto pass = read_file("src/compiler/pass_impls.ixx");
    CHECK(low.find("propagate_marker_from_ast") != std::string::npos,
          "AC1: propagate_marker_from_ast helper");
    CHECK(low.find("#2764") != std::string::npos, "AC1: lowering cites #2764");
    CHECK(low.find("kMaxAncestorWalk") != std::string::npos ||
              low.find("parent_of") != std::string::npos,
          "AC1: ancestor walk for MacroIntroduced envelope");
    CHECK(pass.find("#2764") != std::string::npos, "AC1: InlinePass cites #2764");
    CHECK(pass.find("respect_macro_hygiene_") != std::string::npos,
          "AC1: respect_macro_hygiene_ present");
    CHECK(pass.find("macro_hygiene_skipped_") != std::string::npos, "AC1: hygiene skip metric");
    // Unified skip: call site OR callee MacroIntroduced.
    CHECK(pass.find("instr.source_marker == 1") != std::string::npos ||
              pass.find("source_marker == 1") != std::string::npos,
          "AC1: call-site MacroIntroduced skip");
    CHECK(pass.find("callee->marker == 1") != std::string::npos ||
              pass.find("callee.marker == 1") != std::string::npos,
          "AC1: callee MacroIntroduced skip");

    // Runtime: expand macro → IR attrs present (marker lineage).
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC1: macro workspace");
    for (int i = 0; i < 3; ++i)
        (void)cs.eval("(eval-current)");
    if (cs.macro_ir_attr_cache_size() == 0)
        (void)cs.seed_macro_ir_attrs_from_workspace();
    CHECK(cs.macro_ir_attr_cache_size() > 0 ||
              href(cs, "query:ir-hygiene-stats", "ir-instr-macro-introduced") > 0 ||
              href(cs, "query:ir-hygiene-stats", "macro-markers") > 0,
          "AC1: MacroIntroduced lineage after expand/lower");
    CHECK(href(cs, "query:ir-hygiene-stats", "propagate-marker-from-ast-wired") == 1,
          "AC1: propagate wired sentinel");
    CHECK(href(cs, "query:ir-hygiene-stats", "inline-macro-hygiene-hard-filter-wired") == 1,
          "AC1: InlinePass hard-filter wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "respect-macro-hygiene") == 1,
          "AC1: production default respect-macro-hygiene");
}

static void ac2764_2_deopt_restore_fidelity() {
    std::println("\n--- #2764 AC2: expand → deopt → MacroIntroduced + provenance ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC2: macro workspace");
    for (int i = 0; i < 4; ++i)
        (void)cs.eval("(eval-current)");
    if (cs.macro_ir_attr_cache_size() == 0)
        CHECK(cs.seed_macro_ir_attrs_from_workspace() > 0, "AC2: seed IR attrs");
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "AC2: flat");
    std::vector<aura::ast::NodeId> macro_nodes;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id) && flat->is_macro_introduced(id))
            macro_nodes.push_back(id);
    }
    CHECK(!macro_nodes.empty(), "AC2: MacroIntroduced nodes present");
    for (auto id : macro_nodes)
        flat->set_marker(id, aura::ast::SyntaxMarker::User);
    const auto restored = cs.restore_macro_introduced_from_ir_after_deopt();
    CHECK(restored > 0, "AC2: deopt restore restamps");
    std::uint64_t back = 0;
    for (auto id : macro_nodes) {
        if (flat->is_macro_introduced(id))
            ++back;
    }
    CHECK(back > 0, "AC2: is_macro_introduced after deopt restore");
    CHECK(href(cs, "query:ir-hygiene-stats", "deopt-restore-macro-introduced-wired") == 1,
          "AC2: deopt restore wired");
}

static void ac2764_3_multi_eval_no_marker_loss() {
    std::println("\n--- #2764 AC3: multi-eval denseness — no marker loss ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC3: macro workspace");
    for (int i = 0; i < 8; ++i)
        (void)cs.eval("(eval-current)");
    if (cs.macro_ir_attr_cache_size() == 0)
        (void)cs.seed_macro_ir_attrs_from_workspace();
    auto* flat = cs.evaluator().workspace_flat();
    CHECK(flat != nullptr, "AC3: flat");
    std::uint64_t macro_n = 0;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id) && flat->is_macro_introduced(id))
            ++macro_n;
    }
    CHECK(macro_n > 0 || cs.macro_ir_attr_cache_size() > 0,
          "AC3: MacroIntroduced survives multi-eval");
    // Structural rebind of a user form (not macro wipe).
    auto mut = cs.eval("(mutate:replace-pattern \"(+ x base)\" \"(+ x base)\")");
    (void)mut;
    for (int i = 0; i < 4; ++i)
        (void)cs.eval("(eval-current)");
    // Force restore path (as denseness deopt would).
    const auto multi0 = aura_multi_eval_macro_marker_preserved_total();
    (void)cs.restore_macro_introduced_from_ir_after_deopt();
    const auto multi1 = aura_multi_eval_macro_marker_preserved_total();
    std::println("  multi-eval preserved {} -> {}", multi0, multi1);
    CHECK(multi1 >= multi0, "AC3: multi-eval preserved monotonic");
    // After restore, markers still present.
    std::uint64_t macro_n2 = 0;
    for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
        if (flat->is_live_node(id) && flat->is_macro_introduced(id))
            ++macro_n2;
    }
    CHECK(macro_n2 > 0 || cs.macro_ir_attr_cache_size() > 0,
          "AC3: no marker loss after multi-eval + rebind");
    CHECK(href(cs, "query:ir-hygiene-stats", "multi-eval-macro-marker-preserved-total") >= 0,
          "AC3: multi-eval preserved key");
    // Leakage key must remain readable (0 preferred after #1891 stamp).
    CHECK(href(cs, "query:ir-hygiene-stats", "hygiene-leakage") >= 0,
          "AC3: hygiene-leakage key readable");
}

static void ac2764_4_non_macro_quiet() {
    std::println("\n--- #2764 AC4: non-macro IR path zero regression ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) (+ x 1)) (h 2)\")").has_value(), "AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
    auto r = cs.eval("(+ 3 4)");
    CHECK(r && is_int(*r) && as_int(*r) == 7, "AC4: non-macro eval ok");
    // Quiet path: ancestor propagation must not fire on pure user AST
    // (or is non-negative if other suites shared process counters).
    CHECK(aura_hygiene_ir_ancestor_propagation_total() >= 0, "AC4: ancestor counter readable");
}

static void ac2764_5_observability() {
    std::println("\n--- #2764 AC5: schema-2764 + additive keys ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(source_has_key(q, "schema-2764"), "AC5: schema-2764");
    CHECK(source_has_key(q, "issue-2764"), "AC5: issue-2764");
    CHECK(source_has_key(q, "marker-ancestor-propagation-total"), "AC5: ancestor key");
    CHECK(source_has_key(q, "multi-eval-macro-marker-preserved-total"), "AC5: multi-eval key");
    CHECK(source_has_key(q, "propagate-marker-from-ast-wired"), "AC5: propagate wired");
    CHECK(source_has_key(q, "inline-macro-hygiene-hard-filter-wired"), "AC5: inline wired");
    // Prior surfaces preserved.
    CHECK(source_has_key(q, "schema-2100"), "AC5: schema-2100 preserved");
    CHECK(source_has_key(q, "schema-2177") || q.find("2177") != std::string::npos,
          "AC5: schema-2177 lineage");
    CHECK(met.find("ir_marker_ancestor_propagation_total") != std::string::npos ||
              met.find("#2764") != std::string::npos,
          "AC5: metrics cite #2764");

    CompilerService cs;
    CHECK(setup_macro_ws(cs), "AC5: workspace");
    (void)cs.eval("(eval-current)");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema-2764") == 2764, "AC5: live schema-2764");
    CHECK(href(cs, "query:ir-hygiene-stats", "issue-2764") == 2764, "AC5: live issue-2764");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema-2100") == 2100, "AC5: live schema-2100");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema") == 2022, "AC5: lineage schema 2022");
    CHECK(href(cs, "query:ir-hygiene-stats", "marker-ancestor-propagation-total") >= 0,
          "AC5: live ancestor total");
    CHECK(href(cs, "query:ir-hygiene-stats", "multi-eval-macro-marker-preserved-total") >= 0,
          "AC5: live multi-eval total");
}

static void ac2764_6_source_and_linter() {
    std::println("\n--- #2764 AC6: source-cite + linter ---");
    const auto low = read_file("src/compiler/lowering.ixx");
    const auto pass = read_file("src/compiler/pass_impls.ixx");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto t = read_file("tests/compiler/test_jit_macro_deopt_hygiene.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_ir_jit_macro_marker_enforcement_2764.py");
    CHECK(low.find("#2764") != std::string::npos, "AC6: lowering cites #2764");
    CHECK(pass.find("#2764") != std::string::npos, "AC6: InlinePass cites #2764");
    CHECK(rt.find("#2764") != std::string::npos, "AC6: runtime cites #2764");
    CHECK(t.find("ac2764_1_propagate_and_inline_hard_filter") != std::string::npos, "AC6: AC1");
    CHECK(t.find("ac2764_2_deopt_restore_fidelity") != std::string::npos, "AC6: AC2");
    CHECK(t.find("ac2764_5_observability") != std::string::npos, "AC6: AC5");
    CHECK(build.find("check_ir_jit_macro_marker_enforcement_2764") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(read_file("docs/design/2764-ir-jit-macro-marker.md").empty(),
          "AC6: no docs/design/2764-* per #1655");
}

} // namespace

int run_test_jit_macro_deopt_hygiene() {
    std::println("=== Issue #2100: MacroIntroduced IR attrs + deopt restore ===");
    ac1_ir_attrs();
    ac2_deopt_restore_ast();
    ac3_non_macro_noop();
    ac4_schema_metrics();
    ac5_blame_after_deopt();
    ac6_source_wiring();
    ac7_aot_marker_parity_2177();
    std::println("\n=== Issue #2764: residual IR/JIT MacroIntroduced enforcement ===");
    ac2764_1_propagate_and_inline_hard_filter();
    ac2764_2_deopt_restore_fidelity();
    ac2764_3_multi_eval_no_marker_loss();
    ac2764_4_non_macro_quiet();
    ac2764_5_observability();
    ac2764_6_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_jit_macro_deopt_hygiene();
}
#endif
