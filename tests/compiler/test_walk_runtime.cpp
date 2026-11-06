// @category: unit
// @reason: Issue #4936 — the deducing-this linear walks and pre_scan run.
//
//   AC1: a Let whose value is Linear is discovered by name, including when
//        that Let is nested under another Let
//   AC2: a Let with no linear op is not discovered, and the linear-op walk
//        returns false
//   AC3: NodeTag::Quote and a Call-head quote do not gensym the inner let
//   AC4: an unquoted let still gensyms to __x_0

#include "test_harness.hpp"

#include "core/transparent_string_hash.hh"

#include <string>
#include <unordered_set>

import std;
import aura.compiler.macro_expansion;
import aura.compiler.type_checker;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::discover_linear_bindings_in_subtree;
using aura::compiler::subtree_has_linear_ops;
using aura::compiler::macro_exp::clone_macro_body;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

void test_linear_walks() {
    std::println("\n--- linear walks ---");
    FlatAST flat;
    StringPool pool;
    const auto x = pool.intern("x");
    const auto y = pool.intern("y");
    const auto lin = flat.add_linear(flat.add_literal(1));
    const auto moved = flat.add_move(flat.add_variable(x));
    const auto inner = flat.add_let(x, lin, moved);
    const auto outer = flat.add_let(y, flat.add_literal(0), inner);
    CHECK(flat.has_linear_ops_live(), "Linear and Move nodes arm the live counter");
    CHECK(subtree_has_linear_ops(flat, outer), "nested Linear is visible from the outer let");
    CHECK(!subtree_has_linear_ops(flat, flat.add_literal(3)), "a literal has no linear op");

    std::unordered_set<std::string> names;
    discover_linear_bindings_in_subtree(flat, pool, outer, names);
    CHECK(names.count("x") == 1, "nested Linear let binding is discovered");
    CHECK(names.count("y") == 0, "the outer non-linear let is not a linear binding");

    FlatAST plain;
    StringPool plain_pool;
    const auto z = plain_pool.intern("z");
    const auto plet = plain.add_let(z, plain.add_literal(2), plain.add_variable(z));
    CHECK(!plain.has_linear_ops_live(), "plain let has no linear op");
    CHECK(!subtree_has_linear_ops(plain, plet), "plain let walk is false");
    std::unordered_set<std::string> none;
    discover_linear_bindings_in_subtree(plain, plain_pool, plet, none);
    CHECK(none.empty(), "plain let discovers nothing");
}

void test_quote_prescan() {
    std::println("\n--- pre_scan quote vs let ---");
    FlatAST src;
    StringPool sp;
    const auto x = sp.intern("x");
    const auto quoted_let = src.add_let(x, src.add_literal(1), src.add_variable(x));
    const auto quoted = src.add_quote(quoted_let);
    FlatAST target;
    StringPool tp;
    NameMap quoted_names;
    const auto cloned = clone_macro_body(target, tp, src, sp, quoted, nullptr, &quoted_names,
                                         SyntaxMarker::MacroIntroduced);
    CHECK(cloned != aura::ast::NULL_NODE, "quoted let clones");
    CHECK(quoted_names.find("x") == quoted_names.end(), "NodeTag::Quote does not gensym x");
    CHECK(std::string(tp.resolve(target.get(target.get(cloned).child(0)).sym_id)) == "x",
          "quoted let binding stays x");

    FlatAST src_call;
    StringPool sp_call;
    const auto x2 = sp_call.intern("x");
    const auto inner = src_call.add_let(x2, src_call.add_literal(1), src_call.add_variable(x2));
    const auto qhead = src_call.add_variable(sp_call.intern("quote"));
    const NodeId qargs[] = {inner};
    const auto qcall = src_call.add_call(qhead, std::span<const NodeId>{qargs});
    FlatAST target_call;
    StringPool tp_call;
    NameMap call_names;
    const auto cloned_call = clone_macro_body(target_call, tp_call, src_call, sp_call, qcall,
                                              nullptr, &call_names, SyntaxMarker::MacroIntroduced);
    CHECK(cloned_call != aura::ast::NULL_NODE, "Call-head quote clones");
    CHECK(call_names.find("x") == call_names.end(), "Call-head quote does not gensym x");

    FlatAST src_let;
    StringPool sp_let;
    const auto x3 = sp_let.intern("x");
    const auto bare = src_let.add_let(x3, src_let.add_literal(1), src_let.add_variable(x3));
    FlatAST target_let;
    StringPool tp_let;
    NameMap let_names;
    const auto cloned_let = clone_macro_body(target_let, tp_let, src_let, sp_let, bare, nullptr,
                                             &let_names, SyntaxMarker::MacroIntroduced);
    CHECK(cloned_let != aura::ast::NULL_NODE, "bare let clones");
    // This hand-built Let stamps __x_0_0 (sym counter plus the per-clone index).
    // A parsed `(let ((tmp 1)) tmp)` still stamps __tmp_0; that case is the
    // #265 suite. Quote above must not stamp either form.
    const auto cloned_name = std::string(tp_let.resolve(target_let.get(cloned_let).sym_id));
    CHECK(let_names.count("x") == 1 && let_names.at("x") == "__x_0_0" && cloned_name == "__x_0_0",
          "bare let gensyms x to __x_0_0");
}

} // namespace

int main() {
    test_linear_walks();
    test_quote_prescan();
    std::println("test_walk_runtime: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
