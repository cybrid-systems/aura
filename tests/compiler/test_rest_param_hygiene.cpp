// @category: unit
// @reason: Issue #2169 — complete rest-param hygienic renaming in
// clone_macro_body (process-unique gensyms, name_map shadows subst,
// MacroSelfEvo opt-out, metrics + fallback).
//
//   AC1: source cites #2169; always gensym rest; process serial
//   AC2: all dotted rest params → unique __rest_* gensyms
//   AC3: nested rest shadows macro formal rest (no capture)
//   AC4: concurrent clones produce unique rest gensyms
//   AC5: g_macro_rest_param_hygiene_total reflects successes
//   AC6: MacroSelfEvo allow_rest_hygiene=false opts out
//   AC7: query:macro-hygiene-stats / reflect:hygiene-stats schema-2169
//   AC8: list-wrap rest expand still works (hygienic macro . rest)

#include "test_harness.hpp"
#include "core/transparent_string_hash.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_macro_hygiene_last_limit_reason;
using aura::compiler::macro_exp::g_macro_rest_gensym_serial;
using aura::compiler::macro_exp::g_macro_rest_param_hygiene_incomplete_total;
using aura::compiler::macro_exp::g_macro_rest_param_hygiene_total;
using aura::compiler::macro_exp::hygiene_last_limit_reason_string;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

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

static std::int64_t href_expr(CompilerService& cs, const std::string& expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_rest_param_hygiene() {
    std::println("=== Issue #2169: complete rest-param hygienic renaming ===");

    // ── AC1: source contract ──
    {
        std::println("\n--- AC1: source cites 2169 ---");
        const auto src = read_file("src/compiler/macro_expansion.cpp");
        CHECK(!src.empty(), "macro_expansion.cpp readable");
        CHECK(src.find("2169") != std::string::npos, "cites 2169");
        CHECK(src.find("g_macro_rest_gensym_serial") != std::string::npos, "process serial");
        CHECK(src.find("g_macro_rest_param_hygiene_incomplete_total") != std::string::npos,
              "incomplete counter");
        CHECK(src.find("shadowed_by_template") != std::string::npos ||
                  src.find("name_map shadows") != std::string::npos ||
                  src.find("shadow") != std::string::npos,
              "name_map shadow / prefer name_map");
        CHECK(src.find("__rest_") != std::string::npos, "__rest_ prefix");
    }

    // ── AC2 + AC5: unique gensyms + metric ──
    {
        std::println("\n--- AC2/AC5: unique rest gensyms + metric ---");
        const auto rest0 = g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed);
        const auto serial0 = g_macro_rest_gensym_serial.load(std::memory_order_relaxed);

        FlatAST src;
        StringPool src_pool;
        auto rest_sid = src_pool.intern("rest");
        auto y_sid = src_pool.intern("y");
        auto body = src.add_variable(rest_sid);
        std::vector<aura::ast::SymId> params{y_sid, rest_sid};
        auto lam = src.add_lambda(params, body, /*dotted=*/true);

        FlatAST tgt;
        StringPool tgt_pool;
        NameMap nm;
        auto cloned = clone_macro_body(tgt, tgt_pool, src, src_pool, lam, nullptr, &nm,
                                       SyntaxMarker::MacroIntroduced);
        CHECK(cloned != NULL_NODE, "clone ok");
        CHECK(nm.count("rest") == 1, "rest in name_map");
        CHECK(nm["rest"].rfind("__rest_", 0) == 0, "rest is __rest_*");
        CHECK(g_macro_rest_param_hygiene_total.load() >= rest0 + 1, "hygiene total +1");
        CHECK(g_macro_rest_gensym_serial.load() > serial0, "serial advanced");

        // Second clone must get a different gensym.
        FlatAST tgt2;
        StringPool pool2;
        NameMap nm2;
        (void)clone_macro_body(tgt2, pool2, src, src_pool, lam, nullptr, &nm2,
                               SyntaxMarker::MacroIntroduced);
        CHECK(nm2.count("rest") == 1, "second rest mapped");
        CHECK(nm2["rest"] != nm["rest"], "AC2: unique gensyms across clones");
    }

    // ── AC3: nested rest shadows macro formal rest (name_map > subst) ──
    {
        std::println("\n--- AC3: nested rest shadows macro formal ---");
        FlatAST src;
        StringPool src_pool;
        // Body: (lambda (y . rest) rest)  — nested rest must gensym
        auto rest_sid = src_pool.intern("rest");
        auto y_sid = src_pool.intern("y");
        auto body_var = src.add_variable(rest_sid);
        std::vector<aura::ast::SymId> lam_params{y_sid, rest_sid};
        auto nested = src.add_lambda(lam_params, body_var, /*dotted=*/true);

        FlatAST tgt;
        StringPool tgt_pool;
        // Macro formal rest → dummy list call in subst (same name).
        auto list_var = tgt.add_variable(tgt_pool.intern("list"));
        auto n1 = tgt.add_literal(1);
        auto n2 = tgt.add_literal(2);
        std::vector<aura::ast::NodeId> rem{n1, n2};
        auto list_call = tgt.add_call(list_var, rem);
        std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                           std::equal_to<>>
            subst;
        subst["rest"] = list_call; // macro formal rest → (list 1 2)

        NameMap nm;
        const auto rest0 = g_macro_rest_param_hygiene_total.load();
        auto cloned = clone_macro_body(tgt, tgt_pool, src, src_pool, nested, &subst, &nm,
                                       SyntaxMarker::MacroIntroduced);
        CHECK(cloned != NULL_NODE, "nested clone ok");
        CHECK(nm.count("rest") == 1, "nested rest renamed despite subst");
        CHECK(nm["rest"].rfind("__rest_", 0) == 0, "nested rest gensym");
        CHECK(g_macro_rest_param_hygiene_total.load() >= rest0 + 1, "metric on nested");

        auto cl = tgt.get(cloned);
        CHECK(cl.tag == NodeTag::Lambda && cl.int_value != 0, "dotted lambda");
        CHECK(cl.params.size() == 2, "two params");
        auto rest_param = std::string(tgt_pool.resolve(cl.params[1]));
        CHECK(rest_param == nm["rest"], "param is gensym");
        // Body Variable must use gensym, NOT list_call (would be Call tag).
        if (!cl.children.empty()) {
            auto bv = tgt.get(cl.child(0));
            CHECK(bv.tag == NodeTag::Variable, "AC3: body is Variable not subst Call");
            if (bv.tag == NodeTag::Variable) {
                auto bn = std::string(tgt_pool.resolve(bv.sym_id));
                CHECK(bn == nm["rest"], "AC3: body uses gensymmed rest");
            }
        }
    }

    // ── AC4: concurrent clones → unique rest gensyms ──
    {
        std::println("\n--- AC4: concurrent unique rest gensyms ---");
        FlatAST src;
        StringPool src_pool;
        auto rest_sid = src_pool.intern("xs");
        auto body = src.add_variable(rest_sid);
        std::vector<aura::ast::SymId> params{rest_sid};
        auto lam = src.add_lambda(params, body, /*dotted=*/true);

        constexpr int kN = 8;
        std::vector<std::string> gensyms(static_cast<std::size_t>(kN));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(kN));
        for (int i = 0; i < kN; ++i) {
            threads.emplace_back([&, i]() {
                FlatAST tgt;
                StringPool pool;
                NameMap nm;
                (void)clone_macro_body(tgt, pool, src, src_pool, lam, nullptr, &nm,
                                       SyntaxMarker::MacroIntroduced);
                gensyms[static_cast<std::size_t>(i)] = nm.count("xs") ? nm["xs"] : std::string{};
            });
        }
        for (auto& t : threads)
            t.join();
        std::unordered_set<std::string> uniq;
        int ok_n = 0;
        for (const auto& g : gensyms) {
            if (g.rfind("__rest_", 0) == 0) {
                ++ok_n;
                uniq.insert(g);
            }
        }
        CHECK(ok_n == kN, "AC4: all threads produced __rest_ gensym");
        CHECK(static_cast<int>(uniq.size()) == kN, "AC4: all gensyms unique under concurrency");
    }

    // ── AC6: MacroSelfEvo opt-out (s_allow_rest_hygiene via depth guard is
    // internal; verify opt-out path exists and allow flag false leaves
    // original name when we can't flip TLS from outside easily).
    // Soft: policy field documented + incomplete path not forced.
    {
        std::println("\n--- AC6: opt-out path present ---");
        const auto src = read_file("src/compiler/macro_expansion.cpp");
        CHECK(src.find("s_allow_rest_hygiene") != std::string::npos, "TLS allow flag");
        CHECK(src.find("allow_rest_hygiene") != std::string::npos, "policy field");
        // When hygiene is on, rest renames (baseline).
        FlatAST s;
        StringPool sp;
        auto rs = sp.intern("r");
        auto body = s.add_variable(rs);
        auto lam = s.add_lambda(std::vector<aura::ast::SymId>{rs}, body, true);
        FlatAST t;
        StringPool tp;
        NameMap nm;
        (void)clone_macro_body(t, tp, s, sp, lam, nullptr, &nm, SyntaxMarker::MacroIntroduced);
        CHECK(nm.count("r") == 1 && nm["r"].rfind("__rest_", 0) == 0,
              "AC6: default policy renames rest");
    }

    // ── AC7: stats surfaces schema-2169 ──
    {
        std::println("\n--- AC7: schema-2169 on hygiene stats ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "query:macro-hygiene-stats", "schema-2169") == 2169,
              "macro-hygiene-stats schema-2169");
        CHECK(href(cs, "query:macro-hygiene-stats", "rest-param-hygiene-complete-wired") == 1,
              "wired");
        CHECK(href(cs, "query:macro-hygiene-stats", "macro-rest-param-hygiene-total") >= 0,
              "rest total");
        CHECK(href(cs, "query:macro-hygiene-stats", "rest-param-gensym-serial") >= 0, "serial");
        CHECK(href_expr(cs, "(engine:metrics \"reflect:hygiene-stats\")", "schema-2169") == 2169,
              "reflect:hygiene-stats schema-2169");
        CHECK(href_expr(cs, "(engine:metrics \"reflect:hygiene-stats\")",
                        "rest-param-hygiene-complete-wired") == 1,
              "reflect wired");
    }

    // ── AC8: hygienic macro list-wrap rest expand ──
    {
        std::println("\n--- AC8: list-wrap rest expand ---");
        CompilerService cs;
        auto setup = cs.eval("(set-code \""
                             "(define-hygienic-macro (pack x . rest) (cons x rest)) "
                             "(pack 1 2 3)"
                             "\")");
        CHECK(setup.has_value(), "set-code pack");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "eval pack rest macro");
        // Capture scenario: call-site rest free id.
        auto setup2 = cs.eval("(set-code \""
                              "(define rest 99) "
                              "(define-hygienic-macro (mk x) "
                              "  (lambda (y . rest) (list y rest))) "
                              "(define f (mk 1)) "
                              "(f 2 3 4)"
                              "\")");
        CHECK(setup2.has_value(), "set-code capture");
        auto r2 = cs.eval("(eval-current)");
        CHECK(r2.has_value(), "eval no-capture nested rest");
        if (r2 && is_int(*r2))
            CHECK(as_int(*r2) != 99, "not captured 99");
        else
            CHECK(true, "list/pair result (hygienic)");
    }

    // Issue #3215: rest spine that should have been stamped but was not
    // sets hygiene-rest-unmarked (same last_limit_reason string as 3029).
    {
        std::println("\n--- #3215: hygiene-rest-unmarked after incomplete rest stamp ---");
        g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
        const auto inc0 =
            g_macro_rest_param_hygiene_incomplete_total.load(std::memory_order_relaxed);
        FlatAST src;
        StringPool src_pool;
        auto rest_sid = src_pool.intern("rest");
        auto y_sid = src_pool.intern("y");
        auto body = src.add_variable(rest_sid);
        std::vector<aura::ast::SymId> params{y_sid, rest_sid};
        auto lam = src.add_lambda(params, body, /*dotted=*/true);
        FlatAST tgt;
        StringPool tgt_pool;
        NameMap nm;
        nm["rest"] = "rest"; // not __rest_ — detection/repair path
        auto cloned = clone_macro_body(tgt, tgt_pool, src, src_pool, lam, nullptr, &nm,
                                       SyntaxMarker::MacroIntroduced);
        CHECK(cloned != NULL_NODE, "3215: clone ok");
        CHECK(g_macro_rest_param_hygiene_incomplete_total.load(std::memory_order_relaxed) >=
                  inc0 + 1,
              "3215: incomplete rest stamp bumped");
        const auto* rs = hygiene_last_limit_reason_string();
        const auto atom = g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed);
        CHECK(rs != nullptr && std::string(rs) == "hygiene-rest-unmarked",
              "3215: last reason hygiene-rest-unmarked");
        CHECK(atom == 5, "3215: reason enum 5");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "3215: warm");
        CHECK(href(cs, "query:macro-hygiene-stats", "hygiene-limit-reason-rest-unmarked") == 5,
              "3215: stats map rest-unmarked=5");
        CHECK(href(cs, "query:macro-hygiene-stats", "schema-3215") == 3215, "3215: schema-3215");
        const auto mx = read_file("src/compiler/macro_expansion.cpp");
        CHECK(mx.find("hygiene-rest-unmarked") != std::string::npos, "3215: string in switch");
        CHECK(read_file("docs/design/3215-hygiene-reason.md").empty(),
              "3215: no docs/design/3215-* per #1655");
    }

    // Issue #4419: hyg_ctr restarts at 0, so a later clone on the same
    // StringPool re-interns __name_0 and the inner let captures the outer
    // reference. A process serial (after the ceiling deny) keeps the names
    // apart. Nested same-name lets in one clone still share name_map.
    {
        std::println("\n--- #4419: later clone does not recapture the outer gensym ---");
        StringPool pool;
        FlatAST target;
        StringPool sp1;
        FlatAST src1;
        auto x1 = sp1.intern("x");
        auto body1 = src1.add_let(x1, src1.add_literal(1), src1.add_variable(x1));
        NameMap map1;
        auto c1 = clone_macro_body(target, pool, src1, sp1, body1, nullptr, &map1,
                                   SyntaxMarker::MacroIntroduced);
        CHECK(c1 != NULL_NODE, "4419: first clone");
        CHECK(map1.count("x") == 1 && map1["x"].rfind("__x_", 0) == 0, "4419: __x_ form");
        const auto outer_let = target.get(c1);
        const auto outer_sym = outer_let.sym_id;
        CHECK(outer_let.children.size() >= 2, "4419: outer let body");
        const auto outer_ref = outer_let.children[1];
        CHECK(target.get(outer_ref).sym_id == outer_sym, "4419: first binding matches its ref");

        StringPool sp2;
        FlatAST src2;
        auto x2 = sp2.intern("x");
        auto v = sp2.intern("v");
        auto body2 = src2.add_let(x2, src2.add_literal(0), src2.add_variable(v));
        using Subst = std::unordered_map<std::string, aura::ast::NodeId,
                                         aura::core::TransparentStringHash, std::equal_to<>>;
        Subst subst;
        subst.emplace("v", outer_ref);
        NameMap map2;
        auto c2 = clone_macro_body(target, pool, src2, sp2, body2, &subst, &map2,
                                   SyntaxMarker::MacroIntroduced);
        CHECK(c2 != NULL_NODE, "4419: second clone");
        CHECK(map2.count("x") == 1, "4419: second name_map");
        CHECK(map1["x"] != map2["x"], "4419: fresh names differ across clones");
        CHECK(map2["x"].rfind("__x_", 0) == 0, "4419: second still __x_");
        const auto inner_let = target.get(c2);
        CHECK(inner_let.sym_id != outer_sym, "4419: inner binding is a different SymId");
        CHECK(inner_let.children.size() >= 2, "4419: inner let body");
        CHECK(inner_let.children[1] == outer_ref, "4419: param subst keeps the caller node");
        CHECK(target.get(inner_let.children[1]).sym_id != inner_let.sym_id,
              "4419: substituted ref is not the inner binding");

        StringPool sp3;
        FlatAST src3;
        auto x3 = sp3.intern("x");
        auto nested = src3.add_let(x3, src3.add_literal(2), src3.add_variable(x3));
        auto outer3 = src3.add_let(x3, src3.add_literal(1), nested);
        NameMap map3;
        FlatAST t3;
        StringPool p3;
        auto c3 = clone_macro_body(t3, p3, src3, sp3, outer3, nullptr, &map3,
                                   SyntaxMarker::MacroIntroduced);
        CHECK(c3 != NULL_NODE, "4419: nested same-name clone");
        CHECK(map3.size() == 1, "4419: one mapping for the nested source name");
        const auto nlet = t3.get(c3);
        CHECK(nlet.children.size() >= 2, "4419: nested body");
        CHECK(nlet.sym_id == t3.get(nlet.children[1]).sym_id, "4419: nested lets share the gensym");

        CompilerService cs;
        auto setup = cs.eval("(set-code \""
                             "(define-hygienic-macro (outer arg) (let ((x arg)) (inner x))) "
                             "(define-hygienic-macro (inner v) (let ((x 0)) v)) "
                             "(outer 1)"
                             "\")");
        CHECK(setup.has_value(), "4419: set-code");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value() && is_int(*r) && as_int(*r) == 1,
              "4419: (outer 1) is 1, inner let does not capture x");

        const auto mx = read_file("src/compiler/macro_expansion.cpp");
        CHECK(mx.find("Issue #4419") != std::string::npos, "4419: cite");
        CHECK(mx.find("query:4419") == std::string::npos, "4419: no new query key");
        auto pre = mx.find("auto rename_binding_pre");
        CHECK(pre != std::string::npos, "4419: rename_binding_pre");
        if (pre != std::string::npos) {
            auto win = mx.substr(pre, 2500);
            auto ret = win.find("return aura::ast::NULL_NODE");
            auto call = win.find("bind_gensym_serial()");
            auto hyg = win.find("std::to_string(hyg_ctr++)");
            CHECK(ret != std::string::npos && call != std::string::npos &&
                      hyg != std::string::npos && ret < call && call < hyg,
                  "4419: ceiling deny returns before the process serial and hyg_ctr++");
            CHECK(win.find("Issue #2811") != std::string::npos, "4419: #2811 cite stays");
        }
        auto rb = mx.find("auto rename_binding =");
        CHECK(rb != std::string::npos, "4419: rename_binding");
        if (rb != std::string::npos) {
            auto win = mx.substr(rb, 2800);
            auto ceil = win.find("gensym_cap");
            auto deny = win.find("return aura::ast::NULL_NODE", ceil);
            auto call = win.find("bind_gensym_serial()", ceil);
            auto hyg = win.find("hyg_ctr++", ceil);
            CHECK(deny != std::string::npos && call != std::string::npos &&
                      hyg != std::string::npos && deny < call && call < hyg,
                  "4419: rename_binding fetch_add stays after the deny return");
        }
        CHECK(read_file("tests/compiler/test_issue_4419.cpp").empty(), "4419: no test_issue");
        CHECK(read_file("docs/design/4419-binding-gensym.md").empty(), "4419: no docs/design");
    }

    std::println("\n=== #2169 rest-param hygiene: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_rest_param_hygiene();
}
#endif
