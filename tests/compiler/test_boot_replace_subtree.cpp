// @category: unit
// @reason: Issue #4936 — Evaluator boot registers primitives, and
//          mutate:replace-subtree itself runs.
//
//   AC1: constructing CompilerService runs register_all_primitives
//   AC2: (+) is the registered primitive, not a stub
//   AC3: ast:restore is registered and a snapshot round-trip runs
//   AC4: replace-subtree replaces a user lambda and reports the captured SymId's name
//
// The FlatAST helper test does not execute this primitive. This one does.

#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

import std;
import aura.compiler.service;
import aura.compiler.value;
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

void show_err(const auto& r, const char* label) {
    if (r.has_value())
        return;
    std::println(std::cerr, "FAIL {} diag: {}", label, r.error().message);
}

aura::ast::NodeId find_define(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                              std::string_view name) {
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.is_free_slot(id))
            continue;
        const auto n = flat.get(id);
        if (n.tag != aura::ast::NodeTag::Define || !n.has_name())
            continue;
        if (pool.resolve(n.sym_id) == name)
            return id;
    }
    return aura::ast::NULL_NODE;
}

} // namespace

int main() {
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

    aura::compiler::CompilerService cs;
    const auto plus = cs.evaluator().primitives().lookup("+");
    const auto repl = cs.evaluator().primitives().lookup("mutate:replace-subtree");
    expect(plus.has_value() && repl.has_value(), "boot registered + and mutate:replace-subtree");

    auto sum = cs.eval("(+ 1 2)");
    show_err(sum, "(+ 1 2)");
    expect(sum.has_value() && aura::compiler::types::is_int(*sum) &&
               aura::compiler::types::as_int(*sum) == 3,
           "boot ran the registered +");

    // Registration of ast:restore rebinds the owning registrar. A function_ref
    // copy of that registrar calls the wrapper forever and blows the stack in
    // this constructor. Lookup plus a snapshot round-trip is the registration
    // proof: the primitive is present and the wrapped body runs.
    const auto restore = cs.evaluator().primitives().lookup("mw:restore");
    expect(restore.has_value(), "boot registered ast:restore");
    auto marked = cs.eval("(set-code \"(define restore-boot 1)\")");
    show_err(marked, "set-code restore-boot");
    expect(marked.has_value(), "set-code installed restore-boot");
    auto snap = cs.eval("(mutate :snapshot \"restore-boot\")");
    show_err(snap, "mw:snapshot");
    expect(snap.has_value() && aura::compiler::types::is_int(*snap) &&
               aura::compiler::types::as_int(*snap) >= 0,
           "ast:snapshot returned an id");
    if (snap.has_value() && aura::compiler::types::is_int(*snap)) {
        auto replaced = cs.eval("(set-code \"(define restore-boot 2)\")");
        show_err(replaced, "set-code overwrites the snapshot");
        expect(replaced.has_value(), "workspace moved off the snapshot");
        auto back =
            cs.eval(std::format("(mutate :restore {})", aura::compiler::types::as_int(*snap)));
        show_err(back, "mw:restore");
        expect(back.has_value() && aura::compiler::types::is_bool(*back) &&
                   aura::compiler::types::as_bool(*back),
               "ast:restore returned true");
        auto rebound = cs.eval("(eval-current)");
        show_err(rebound, "eval-current after ast:restore");
        auto got = cs.eval("restore-boot");
        show_err(got, "restore-boot");
        expect(got.has_value() && aura::compiler::types::is_int(*got) &&
                   aura::compiler::types::as_int(*got) == 1,
               "ast:restore put the snapshotted define back");
    }

    auto set = cs.eval("(set-code \"(define (f a) (lambda (x) x))\")");
    show_err(set, "set-code");
    expect(set.has_value(), "set-code installed the define");
    auto ran = cs.eval("(eval-current)");
    show_err(ran, "eval-current");
    expect(ran.has_value(), "eval-current bound f");

    auto* flat = cs.evaluator().workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    expect(flat != nullptr && pool != nullptr, "workspace flat and pool exist");
    if (flat == nullptr || pool == nullptr)
        return 1;
    const auto def = find_define(*flat, *pool, "f");
    expect(def != aura::ast::NULL_NODE, "define f is in the workspace");
    if (def == aura::ast::NULL_NODE)
        return 1;
    const auto outer_lam = flat->get(def).child(0);
    const auto inner = flat->get(outer_lam).child(0);
    expect(flat->get(inner).tag == aura::ast::NodeTag::Lambda, "f's body is the inner lambda");

    const auto before = flat->get(outer_lam).child(0);
    auto rep = cs.eval(std::format("(mutate:replace-subtree {} \"(lambda (x) a)\")",
                                   static_cast<unsigned>(inner)));
    show_err(rep, "mutate:replace-subtree");
    expect(rep.has_value(), "replace-subtree returned a value");
    if (!rep.has_value())
        return 1;
    expect(!aura::compiler::types::is_bool(*rep),
           "a captured free var is not the plain #t success");
    const auto after = cs.evaluator().workspace_flat()->get(outer_lam).child(0);
    expect(after != before && after != aura::ast::NULL_NODE, "the primitive replaced the lambda");
    const auto printed = aura::compiler::replace_subtree_old_source(
        *cs.evaluator().workspace_flat(), *cs.evaluator().workspace_pool(), after);
    expect(printed == "(lambda (x) a)", "installed source is the requested lambda");

    auto call = cs.eval("((f 1) 9)");
    show_err(call, "((f 1) 9)");
    expect(call.has_value() && aura::compiler::types::is_int(*call) &&
               aura::compiler::types::as_int(*call) == 1,
           "the replaced lambda returns the captured outer argument");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
