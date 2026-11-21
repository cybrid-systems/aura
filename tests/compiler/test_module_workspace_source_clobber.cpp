// @category: unit
// @reason: Issue #5384 — a module define with the same name as a workspace
//          define must not clobber the workspace's canonical source row.
//          cache_module recorded the WHOLE module text under the name; the
//          specialize pass scoped lowering to the named define but the
//          #4621 rest gate read the FIRST root define, so a rest-param
//          workspace define was IR-published from the module and its
//          closure resolved to a sibling function ("invalid closure
//          reference" / foreign arity after a rebind).
//
//   AC1: workspace rest-param define + same-name module define; after
//        eval-current the workspace rest semantics survive (rest is a list)
//   AC2: a caller of the workspace define still dispatches after a rebind
//        of an unrelated workspace define (no invalid closure / foreign arity)
//   AC3: the module's own same-name define keeps its own behavior

#include "core/runtime_config.hh"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static bool write_mod(const std::string& dir, const std::string& name, const std::string& body) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir + "/" + name);
    if (!out)
        return false;
    out << body;
    return true;
}

static bool eval_ok(CompilerService& cs, std::string_view e) {
    auto r = cs.eval(e);
    return r.has_value() && !is_error(*r);
}

static bool eval_int_eq(CompilerService& cs, std::string_view e, std::int64_t n) {
    auto r = cs.eval(e);
    return r && is_int(*r) && as_int(*r) == n;
}

// The module: a fixed-arity sibling FIRST (the define the unscoped rest
// gate used to read), then the same-name rest-param define the workspace
// also owns. Requiring it records the whole module text under "wmk-piece".
static const char* kModule = R"AURA((export wmk-sibling wmk-piece)
(define (wmk-sibling a b c d) (+ a b c d))
(define (wmk-piece k x w h y . rest)
  (if (null? rest) (list k x w h y) (cons k rest)))
)AURA";

// The workspace: the rest-param define plus a caller and an unrelated
// define to rebind. The caller goes through the workspace define's cell.
static const char* kWorkspace = R"AURA(
(define (wmk-piece k x w h y . rest)
  (if (null? rest) (list k x w h y) (cons k rest)))
(define (wmk-use) (wmk-piece 1 2 3 4 5 6 7))
(define (wmk-other n) (+ n 1))
)AURA";

static void ac1_rest_semantics_survive(const std::string& mod_dir) {
    std::println("\n--- #5384 AC1: workspace rest semantics survive the module require ---");
    CompilerService cs;
    CHECK(eval_ok(cs, std::string("(set-code \"") + kWorkspace + "\")"), "set-code workspace");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current workspace");
    CHECK(eval_ok(cs, std::string("(require \"") + mod_dir + "/wmkmod\" all:)"), "require module");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current after require");
    // rest is the pair list (6 7), not the first extra arg as-is.
    CHECK(eval_int_eq(cs, "(length (wmk-piece 1 2 3 4 5 6 7))", 3),
          "AC1: rest binds as a list (cons k rest)");
    CHECK(eval_int_eq(cs, "(car (cdr (wmk-piece 1 2 3 4 5 6 7)))", 6),
          "AC1: rest elements reachable");
    CHECK(eval_int_eq(cs, "(length (wmk-piece 1 2 3 4 5))", 5),
          "AC1: empty rest keeps the named args");
}

static void ac2_caller_dispatches_after_rebind(const std::string& mod_dir) {
    std::println("\n--- #5384 AC2: caller dispatch after an unrelated rebind ---");
    CompilerService cs;
    CHECK(eval_ok(cs, std::string("(set-code \"") + kWorkspace + "\")"), "set-code workspace");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current workspace");
    CHECK(eval_ok(cs, std::string("(require \"") + mod_dir + "/wmkmod\" all:)"), "require module");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current after require");
    CHECK(eval_int_eq(cs, "(length (wmk-use))", 3), "AC2: caller before rebind");
    CHECK(eval_ok(cs, "(mutate:rebind \"wmk-other\" \"(define (wmk-other n) (+ n 2))\" \"test\")"),
          "rebind unrelated define");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current after rebind");
    // Before the fix this died with "invalid closure reference" (the
    // workspace piece closure resolved to the module's wmk-sibling).
    CHECK(eval_int_eq(cs, "(length (wmk-use))", 3),
          "AC2: caller after rebind (no invalid closure)");
    CHECK(eval_int_eq(cs, "(wmk-other 1)", 3), "AC2: rebound define uses the new body");
}

static void ac3_module_define_keeps_behavior(const std::string& mod_dir) {
    std::println("\n--- #5384 AC3: module's own same-name define keeps its behavior ---");
    CompilerService cs;
    CHECK(eval_ok(cs, std::string("(require \"") + mod_dir + "/wmkmod\" all:)"), "require module");
    CHECK(eval_int_eq(cs, "(wmk-sibling 1 2 3 4)", 10), "AC3: module sibling callable");
    CHECK(eval_int_eq(cs, "(length (wmk-piece 1 2 3 4 5 6 7))", 3),
          "AC3: module rest define binds rest as a list");
}

} // namespace

int run_test_module_workspace_source_clobber() {
    std::println("=== Issue #5384: module source row must not clobber workspace define ===");
    ::aura::config::test_setenv("AURA_SANDBOX", "off", 1);
    char tmpl[] = "/tmp/aura_test_wsrc5384_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir) {
        std::println(stderr, "mkdtemp failed");
        return 1;
    }
    const std::string mod_dir(dir);
    CHECK(write_mod(mod_dir, "wmkmod.aura", kModule), "write module");
    ac1_rest_semantics_survive(mod_dir);
    ac2_caller_dispatches_after_rebind(mod_dir);
    ac3_module_define_keeps_behavior(mod_dir);
    std::error_code ec;
    std::filesystem::remove_all(mod_dir, ec);
    ::aura::config::test_unsetenv("AURA_SANDBOX");
    std::println("\n=== #5384: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_workspace_source_clobber();
}
#endif
