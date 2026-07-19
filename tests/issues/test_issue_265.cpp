// @category: unit
// @reason: isolated clone_macro_body hygiene tests (Issue #265)


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.macro_expansion;
import aura.parser.parser;

namespace aura_issue_265_detail {

struct FlatEnv {
    std::unique_ptr<aura::ast::ASTArena> arena;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
};

FlatEnv make_env() {
    FlatEnv e;
    e.arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = e.arena->allocator();
    e.flat = e.arena->create<aura::ast::FlatAST>(alloc);
    e.pool = e.arena->create<aura::ast::StringPool>(alloc);
    return e;
}

aura::ast::NodeId parse(FlatEnv& e, const std::string& src) {
    auto pr = aura::parser::parse_to_flat(src, *e.flat, *e.pool);
    e.flat->root = pr.root;
    return pr.root;
}

static std::string sym_name(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                            aura::ast::NodeId id) {
    auto v = flat.get(id);
    if (v.sym_id == aura::ast::INVALID_SYM)
        return {};
    return std::string(pool.resolve(v.sym_id));
}

static void collect_var_names(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                              aura::ast::NodeId id, std::vector<std::string>& out) {
    if (id == aura::ast::NULL_NODE || id >= flat.size())
        return;
    auto v = flat.get(id);
    if (v.tag == aura::ast::NodeTag::Variable)
        out.push_back(sym_name(flat, pool, id));
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        collect_var_names(flat, pool, v.child(i), out);
}

bool test_hyg_ctr_resets_per_call() {
    std::println("\n--- AC1: hyg_ctr is per clone_macro_body call ---");
    auto src = make_env();
    auto body = parse(src, "(let ((tmp 1)) tmp)");
    auto tgt1 = make_env();
    auto tgt2 = make_env();
    std::unordered_map<std::string, std::string> map1;
    std::unordered_map<std::string, std::string> map2;
    (void)aura::compiler::macro_exp::clone_macro_body(*tgt1.flat, *tgt1.pool, *src.flat, *src.pool,
                                                      body, nullptr, &map1,
                                                      aura::ast::SyntaxMarker::MacroIntroduced);
    (void)aura::compiler::macro_exp::clone_macro_body(*tgt2.flat, *tgt2.pool, *src.flat, *src.pool,
                                                      body, nullptr, &map2,
                                                      aura::ast::SyntaxMarker::MacroIntroduced);
    CHECK(map1.count("tmp") == 1, "first expansion gensyms tmp");
    CHECK(map2.count("tmp") == 1, "second expansion gensyms tmp");
    CHECK(map1.at("tmp") == "__tmp_0", "first expansion uses __tmp_0");
    CHECK(map2.at("tmp") == "__tmp_0",
          "second expansion also starts at __tmp_0 (not global static)");
    return true;
}

bool test_prescan_inner_reference_matches_binding() {
    std::println("\n--- AC2: pre-scan rewrites inner references to gensym ---");
    auto src = make_env();
    auto body = parse(src, "(let ((tmp x)) tmp)");
    auto tgt = make_env();
    std::unordered_map<std::string, std::string> rename_map;
    auto cloned = aura::compiler::macro_exp::clone_macro_body(
        *tgt.flat, *tgt.pool, *src.flat, *src.pool, body, nullptr, &rename_map,
        aura::ast::SyntaxMarker::MacroIntroduced);
    CHECK(cloned != aura::ast::NULL_NODE, "clone succeeded");
    std::vector<std::string> vars;
    collect_var_names(*tgt.flat, *tgt.pool, cloned, vars);
    CHECK(vars.size() >= 2, "let + reference produce >= 2 variable nodes");
    const auto gensym = rename_map.at("tmp");
    int tmp_gensym_hits = 0;
    for (const auto& n : vars) {
        if (n == gensym)
            ++tmp_gensym_hits;
    }
    CHECK(tmp_gensym_hits >= 1, "inner reference uses gensym name from pre-scan");
    auto let_v = tgt.flat->get(cloned);
    CHECK(let_v.tag == aura::ast::NodeTag::Let, "cloned root is let");
    CHECK(std::string(tgt.pool->resolve(let_v.sym_id)) == gensym,
          "let binding sym_id uses gensym from rename_map");
    return true;
}

bool test_builtins_whitelist_not_gensymd() {
    std::println("\n--- AC3: builtins whitelist is not gensym'd ---");
    auto src = make_env();
    auto body = parse(src, "(lambda (if) if)");
    auto tgt = make_env();
    std::unordered_map<std::string, std::string> rename_map;
    auto cloned = aura::compiler::macro_exp::clone_macro_body(
        *tgt.flat, *tgt.pool, *src.flat, *src.pool, body, nullptr, &rename_map,
        aura::ast::SyntaxMarker::MacroIntroduced);
    CHECK(cloned != aura::ast::NULL_NODE, "clone with builtin lambda param succeeded");
    CHECK(rename_map.find("if") == rename_map.end(), "builtin param `if` not gensym'd");
    auto lam = tgt.flat->get(cloned);
    CHECK(lam.tag == aura::ast::NodeTag::Lambda && !lam.params.empty(), "lambda cloned");
    CHECK(std::string(tgt.pool->resolve(lam.params[0])) == "if", "lambda param remains plain `if`");
    return true;
}

bool test_separate_expansions_get_distinct_gensyms_in_one_map() {
    std::println("\n--- AC4: nested bindings get distinct gensyms within one call ---");
    auto src = make_env();
    auto body = parse(src, "(let ((a 1)) (let ((b 2)) (+ a b)))");
    auto tgt = make_env();
    std::unordered_map<std::string, std::string> rename_map;
    (void)aura::compiler::macro_exp::clone_macro_body(*tgt.flat, *tgt.pool, *src.flat, *src.pool,
                                                      body, nullptr, &rename_map,
                                                      aura::ast::SyntaxMarker::MacroIntroduced);
    CHECK(rename_map.at("a") == "__a_0", "first binding is __a_0");
    CHECK(rename_map.at("b") == "__b_1", "second binding is __b_1");
    return true;
}

int run_tests() {
    std::println("Issue #265 (clone_macro_body hygiene module)\n");
    test_hyg_ctr_resets_per_call();
    test_prescan_inner_reference_matches_binding();
    test_builtins_whitelist_not_gensymd();
    test_separate_expansions_get_distinct_gensyms_in_one_map();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_265_detail

int aura_issue_265_run() {
    return aura_issue_265_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_265_run();
}
#endif