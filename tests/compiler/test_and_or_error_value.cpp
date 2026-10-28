// @category: unit
// @reason: Issue #4735 — a primitive type error in a NON-LAST and/or operand is
//          an error VALUE, not a truth value. The short-circuit prefix loop
//          returned it only on the falsy/truthy test, so an error flowed on as
//          truthy ("(and (> 5 \"1\") 'reached) => reached") and an enclosing
//          try never caught it. The last operand is tail-called and already
//          propagated. The caught face also printed the legacy stderr line
//          (#4535 contract: only the UNCAUGHT face prints).
//
//   AC1: (and (> 5 "1") 'reached) raises — try/catch sees the type error
//   AC2: (and (car 5) 'reached) raises (same error-value class)
//   AC3: or-first / if-test / unbound-in-and / and-last still raise (parity)
//   AC4: the caught face prints NOTHING on stderr; the uncaught face still
//        prints the legacy "type mismatch" line once
//   AC5: source-cite — evaluator_eval_flat.cpp + evaluator_primitives_builtins.cpp

#include "test_harness.hpp"

#include <cstdio>
#include <fstream>
#include <print>
#include <string>
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

// Run fn with fd 2 redirected to a temp file; return what it wrote.
template <typename Fn> static std::string capture_stderr(Fn&& fn) {
    ::fflush(nullptr);
    const int saved = ::dup(2);
    char tmpl[] = "/tmp/aura_4735_stderr_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0 || saved < 0)
        return {};
    ::dup2(fd, 2);
    fn();
    ::fflush(nullptr);
    ::dup2(saved, 2);
    ::close(saved);
    ::lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    ::unlink(tmpl);
    return out;
}

static bool eval_bool(CompilerService& cs, const char* src, const char* what) {
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && as_bool(*r);
    if (!ok)
        std::println("  [{}] result was not #t", what);
    return ok;
}

// ── AC1 + AC2: an error value in a non-last and/or operand raises ─────
// The catch clause returns a STRING sentinel: symbol identity is pool-local
// in Aura, so a quoted-symbol compare would miss across the eval pools.
static bool raises(CompilerService& cs, const char* expr, const char* what) {
    std::string src = "(equal? (try ";
    src += expr;
    src += " (catch (e) \"raised\")) \"raised\")";
    return eval_bool(cs, src.c_str(), what);
}

static void ac1_and_or_prefix_error_raises() {
    std::println("\n--- #4735 AC1: error in a non-last and/or operand raises ---");
    CompilerService cs;
    CHECK(raises(cs, "(and (> 5 \"1\") 'reached)", "and-first"),
          "AC1: (and (> 5 \"1\") 'reached) raises");
    // Sanity: the same expression with a legal comparison keeps short-circuiting.
    CHECK(eval_bool(cs, "(equal? (and (> 5 1) 'reached) 'reached)", "and-hot"),
          "AC1: a legal and prefix still reaches the last clause");
    CHECK(eval_bool(cs, "(equal? (and #f 'reached) #f)", "and-false"),
          "AC1: a falsy and prefix still short-circuits");

    std::println("\n--- #4735 AC2: car of a non-pair in an and prefix raises ---");
    CHECK(raises(cs, "(and (car 5) 'reached)", "car-in-and"), "AC2: (and (car 5) 'reached) raises");
}

// ── AC3: the other control forms keep their raising face ─────────────
static void ac3_sibling_forms_parity() {
    std::println("\n--- #4735 AC3: or / if / last-and / unbound parity ---");
    CompilerService cs;
    CHECK(raises(cs, "(or (> 5 \"1\") 'reached)", "or-first"), "AC3: or-first raises");
    CHECK(raises(cs, "(if (> 5 \"1\") 'then 'else)", "if-test"), "AC3: if test raises");
    CHECK(raises(cs, "(and #t (> 5 \"1\"))", "and-last"), "AC3: and-last raises");
    CHECK(raises(cs, "(and (no-such-fn 1) 'reached)", "unbound"),
          "AC3: unbound in an and prefix raises");
}

// ── AC4: stderr belongs to the uncaught face only (#4535 contract) ────
static void ac4_stderr_face() {
    std::println("\n--- #4735 AC4: caught face is silent, uncaught still prints ---");
    CompilerService cs;
    const auto caught = capture_stderr(
        [&] { (void)cs.eval("(try (and (> 5 \"1\") 'reached) (catch (e) 'raised))"); });
    CHECK(caught.empty(), "AC4: a caught and/type-error writes nothing on stderr");
    if (!caught.empty())
        std::println("  [stderr] {}", caught);

    const auto uncaught = capture_stderr([&] { (void)cs.eval("(and (> 5 \"1\") 'reached)"); });
    CHECK(uncaught.find("type mismatch") != std::string::npos,
          "AC4: the uncaught face still prints the legacy type-mismatch line");
}

// ── AC5: source-cite ─────────────────────────────────────────────────
static void ac5_source_cite() {
    std::println("\n--- #4735 AC5: source cites #4735 ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#4735") != std::string::npos, "AC5: and/or short-circuit path cites #4735");
    CHECK(flat.find("is_error(*ar) && !is_string(*ar)") != std::string::npos,
          "AC5: the prefix loop propagates error values");
    const auto builtins = read_file("src/compiler/evaluator_primitives_builtins.cpp");
    CHECK(builtins.find("#4735") != std::string::npos,
          "AC5: the typed-arg stderr face cites #4735");
    CHECK(builtins.find("try_body_depth") != std::string::npos,
          "AC5: the legacy line is gated on the per-body try depth");
}

} // namespace

int run_test_and_or_error_value() {
    std::println("=== Issue #4735: and/or error-value propagation ===");
    ac1_and_or_prefix_error_raises();
    ac3_sibling_forms_parity();
    ac4_stderr_face();
    ac5_source_cite();
    std::println("\n=== #4735: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_and_or_error_value();
}
#endif
