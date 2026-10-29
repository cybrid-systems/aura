// @category: unit
// @reason: Issue #4743 — quote must return its datum unchanged. The parser
//          desugared the operand of quote like code: (quote (define (f x) x))
//          came back as the lambda form, (quote (let ((a 1)) a)) as a
//          flattened binding pair, (quote (cond (a 1))) EVALUATED to 1 (a is
//          unbound, so not even correct evaluation), and (quote (if a b c))
//          HUNG the evaluator. Desugaring is for code the evaluator runs;
//          the operand of quote / ' is data.
//
//   AC1: (quote (define (f x) x)) is the three-element datum
//   AC2: (quote (let ((a 1)) a)) is the nested-binding datum
//   AC3: (quote (cond (a 1))) is the cond datum, never 1
//   AC4: 'x reader form has the same datum shape
//   AC5: (quote (if a b c)) / (quote (cond (a 1) (else 2))) return (no hang)
//   AC6: eval of the same quoted datum agrees with the direct datum
//   AC7: source-cite — parser_impl.cpp cites #4743 at the datum boundary

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
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

static bool eval_true(CompilerService& cs, const char* src, const char* what) {
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && as_bool(*r);
    if (!ok)
        std::println("  [{}] result was not #t", what);
    return ok;
}

// A datum assertion that must be #f (e.g. "not the desugared shape").
static bool eval_false(CompilerService& cs, const char* src, const char* what) {
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && !as_bool(*r);
    if (!ok)
        std::println("  [{}] result was not #f", what);
    return ok;
}

// Run one .aura file through the CLI with a hard timeout so a hang is a
// failed AC, not a stuck test binary.
struct CliRun {
    int rc = -1;
    std::string out;
    std::string err;
    bool timed_out = false;
};

static CliRun run_cli(const std::string& bin, const std::string& path, int timeout_s) {
    CliRun r;
    const std::string outp = "/tmp/aura-4743-out.txt";
    const std::string errp = "/tmp/aura-4743-err.txt";
    const std::string cmd = "timeout " + std::to_string(timeout_s) +
                            " env AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 '" + bin + "' '" + path +
                            "' >'" + outp + "' 2>'" + errp + "'";
    const int raw = std::system(cmd.c_str());
    r.timed_out = (raw != -1 && WIFEXITED(raw) && WEXITSTATUS(raw) == 124);
    r.rc = (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
    {
        std::ifstream in(outp);
        r.out.assign(std::istreambuf_iterator<char>(in), {});
    }
    {
        std::ifstream in(errp);
        r.err.assign(std::istreambuf_iterator<char>(in), {});
    }
    return r;
}

static std::string aura_binary() {
    if (const char* e = std::getenv("AURA_BIN")) {
        if (::access(e, X_OK) == 0)
            return e;
    }
    char buf[4096];
    const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir(buf);
        const auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            auto cand = dir.substr(0, slash + 1) + "aura";
            if (::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    for (const char* c : {"./aura", "./build/aura", "../aura", "../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

// ── AC1–AC4: the datum keeps its surface shape ──────────────────────
static void ac1_quote_define_datum() {
    std::println("\n--- #4743 AC1: (quote (define (f x) x)) is the datum ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? (quote (define (f x) x)) (list 'define (list 'f 'x) 'x))",
                    "quote define"),
          "AC1: quoted define-form is (define (f x) x) data");
    CHECK(eval_false(cs, "(equal? (quote (define (f x) x)) (list 'define 'f 'lambda))",
                     "quote define not desugared"),
          "AC1: the datum is not the desugared lambda shape");
}

static void ac2_quote_let_datum() {
    std::println("\n--- #4743 AC2: (quote (let ((a 1)) a)) is the datum ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? (quote (let ((a 1)) a)) (list 'let (list (list 'a 1)) 'a))",
                    "quote let"),
          "AC2: quoted let-form keeps its binding list and body");
}

static void ac3_quote_cond_not_evaluated() {
    std::println("\n--- #4743 AC3: (quote (cond (a 1))) is data, not 1 ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? (quote (cond (a 1))) (list 'cond (list 'a 1)))", "quote cond"),
          "AC3: quoted cond-form is data (never evaluated)");
}

static void ac4_reader_quote_same_shape() {
    std::println("\n--- #4743 AC4: 'x reader form has the datum shape ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? '(define (f x) x) (quote (define (f x) x)))", "reader vs quote"),
          "AC4: ' form and (quote ...) agree");
    CHECK(eval_true(cs, "(symbol? (car '(if a b c)))", "reader car symbol"),
          "AC4: ' form keeps a symbol head");
}

// ── AC5: no hang on the forms that used to block forever ────────────
static void ac5_no_hang_on_if_and_cond() {
    std::println("\n--- #4743 AC5: quoted if/cond return instead of hanging ---");
    const auto bin = aura_binary();
    CHECK(!bin.empty(), "AC5: aura binary found");
    if (bin.empty())
        return;
    const std::string path = "/tmp/aura-4743-no-hang.aura";
    {
        std::ofstream f(path);
        f << "(display (quote (if a b c))) (newline)\n";
        f << "(display (quote (cond (a 1) (else 2)))) (newline)\n";
    }
    auto run = run_cli(bin, path, 20);
    if (run.timed_out || run.rc != 0) {
        std::println(std::cerr, "4743 AC5 rc {} timeout {} out [{}] err [{}]", run.rc,
                     run.timed_out, run.out, run.err);
    }
    CHECK(!run.timed_out, "AC5: (quote (if a b c)) does not hang");
    CHECK(run.rc == 0, "AC5: the program exits 0");
    CHECK(run.out.find("(if a b c)") != std::string::npos, "AC5: quoted if-form prints as data");
    CHECK(run.out.find("(cond (a 1) (else 2))") != std::string::npos,
          "AC5: quoted cond-form prints as data");
}

// ── AC6: the string face agrees with the datum face ─────────────────
static void ac6_eval_string_agrees() {
    std::println("\n--- #4743 AC6: (eval \"(quote ...)\") agrees ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? (eval \"(quote (define (f x) x))\") (quote (define (f x) x)))",
                    "eval string define"),
          "AC6: eval of the string form yields the same datum");
}

// ── AC7: source cite ───────────────────────────────────────────────
static void ac7_source_cite() {
    std::println("\n--- #4743 AC7: parser cites the datum boundary ---");
    const auto parser = read_file("src/parser/parser_impl.cpp");
    CHECK(!parser.empty(), "AC7: parser_impl.cpp readable");
    CHECK(parser.find("#4743") != std::string::npos,
          "AC7: parser_impl.cpp cites #4743 at the quote datum boundary");
}

} // namespace

int run_test_quote_datum_forms() {
    std::println("=== Issue #4743: quote protects its datum ===");
    ac1_quote_define_datum();
    ac2_quote_let_datum();
    ac3_quote_cond_not_evaluated();
    ac4_reader_quote_same_shape();
    ac5_no_hang_on_if_and_cond();
    ac6_eval_string_agrees();
    ac7_source_cite();
    std::println("\n=== #4743: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_quote_datum_forms();
}
#endif
