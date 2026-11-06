// @category: unit
// @reason: Issue #4936 B4 — replace-subtree capture and rollback printer run.
//
//   AC1: a free var bound by an outer lambda or let is reported once
//   AC2: a binding inside the new subtree, a builtin, and an unbound name are not
//   AC3: the rollback printer reconstructs a nested lambda call
//   AC4: a Let, which that printer does not special-case, stays a node tag

import std;
import aura.core.ast;

#include "compiler/replace_subtree_capture.hh"

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::StringPool;
using aura::ast::SymId;

} // namespace

int main() {
    FlatAST flat;
    StringPool pool;
    const SymId a = pool.intern("a");
    const SymId b = pool.intern("b");
    const SymId x = pool.intern("x");
    const SymId plus = pool.intern("+");
    const SymId unbound = pool.intern("free");
    const auto lit0 = flat.add_literal(0);
    const auto lit1 = flat.add_literal(1);
    const auto vx = flat.add_variable(x);
    const auto va = flat.add_variable(a);
    const auto vb = flat.add_variable(b);
    const auto vplus = flat.add_variable(plus);
    const auto vfree = flat.add_variable(unbound);
    const NodeId args[] = {vx, va, vb, vfree};
    const auto call = flat.add_call(vplus, std::span<const NodeId>{args});
    const auto inner = flat.add_let(x, lit1, call);
    const auto outer = flat.add_let(b, lit0, inner);
    const SymId lam_params[] = {a};
    const auto lam = flat.add_lambda(std::span<const SymId>{lam_params}, outer);
    (void)lam;

    std::unordered_set<SymId> builtins;
    builtins.insert(plus);
    const auto captured = aura::compiler::replace_subtree_captured_syms(flat, builtins, inner);
    expect(captured.size() == 2 && captured[0] == a && captured[1] == b,
           "outer lambda param then outer let name, once each");
    expect(std::find(captured.begin(), captured.end(), x) == captured.end(),
           "the new subtree's own let binding is not captured");
    expect(std::find(captured.begin(), captured.end(), plus) == captured.end(),
           "builtin + is not captured");
    expect(std::find(captured.begin(), captured.end(), unbound) == captured.end(),
           "unbound free is not captured");

    const auto lone = aura::compiler::replace_subtree_captured_syms(flat, builtins, lit0);
    expect(lone.empty(), "a literal captures nothing");

    FlatAST src;
    StringPool sp;
    const SymId p = sp.intern("a");
    const auto body_var = src.add_variable(p);
    const auto one = src.add_literal(1);
    const NodeId call_args[] = {one};
    const auto body = src.add_call(body_var, std::span<const NodeId>{call_args});
    const SymId ps[] = {p};
    const auto printed_lam = src.add_lambda(std::span<const SymId>{ps}, body);
    expect(aura::compiler::replace_subtree_old_source(src, sp, printed_lam) == "(lambda (a) (a 1))",
           "rollback printer reconstructs a nested lambda call");
    const auto defined = src.add_define(p, one);
    expect(aura::compiler::replace_subtree_old_source(src, sp, defined) == "(define a 1)",
           "rollback printer reconstructs a define");
    const auto let_node = src.add_let(p, one, body_var);
    const auto let_text = aura::compiler::replace_subtree_old_source(src, sp, let_node);
    expect(let_text.starts_with("#<node-"), "a Let stays the unspecialized node tag");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
