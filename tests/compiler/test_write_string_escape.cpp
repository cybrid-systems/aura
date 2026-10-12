// @category: unit
// @reason: Issue #2574 — Scheme write must escape string contents
//          (quotes, backslash, common controls); display stays raw.
//
//   AC1: (write "a\"b") → "a\"b" under default JIT path
//   AC2: (write "a\\b") → "a\\b"
//   AC3: (display "a\"b") → a"b (raw, no escapes)
//   AC4: top-level TW write matches JIT define body
//   AC5: source-cite + cmake + gate

#include "test_harness.hpp"
#include "compiler/pipeline_policy.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_float;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_string;
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

template <typename F> static std::string with_stdout_capture(F&& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    int saved = ::dup(STDOUT_FILENO);
    if (saved < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::fflush(stdout);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::close(saved);
        return {};
    }
    ::close(pipefd[1]);

    fn();

    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);

    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    return out;
}

// Strip trailing newline for comparisons.
static std::string trim_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static void ac1_write_embedded_quote() {
    std::println("\n--- #2574 AC1: write embedded quote (JIT define) ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write "a\"b")
    (newline)
    #t)
  (f)))");
    });
    // Expected external form: "a\"b"
    const bool ok = trim_nl(out) == "\"a\\\"b\"";
    CHECK(ok, "AC1: write \"a\\\"b\" → \"a\\\"b\"");
    if (!ok)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac2_write_backslash() {
    std::println("\n--- #2574 AC2: write backslash ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write "a\\b")
    (newline)
    #t)
  (f)))");
    });
    // Content a\b → external "a\\b"
    const bool ok = trim_nl(out) == "\"a\\\\b\"";
    CHECK(ok, "AC2: write \"a\\\\b\" → \"a\\\\b\"");
    if (!ok)
        std::println(stderr, "  captured: '{}'", out);
}

static void ac3_display_raw() {
    std::println("\n--- #2574 AC3: display stays raw ---");
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display "a\"b")
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "a\"b", "AC3: display prints raw a\"b");
    if (trim_nl(out) != "a\"b")
        std::println(stderr, "  captured: '{}'", out);
}

static void ac4_tw_matches_jit() {
    std::println("\n--- #2574 AC4: top-level TW write matches define/JIT ---");
    CompilerService cs_tw;
    std::string tw =
        with_stdout_capture([&] { (void)cs_tw.eval(R"((begin (write "a\"b") (newline)))"); });
    CompilerService cs_jit;
    std::string jit = with_stdout_capture([&] {
        (void)cs_jit.eval(R"((begin
  (define (f) (write "a\"b") (newline) #t)
  (f)))");
    });
    CHECK(trim_nl(tw) == trim_nl(jit), "AC4: TW and JIT write agree");
    CHECK(trim_nl(tw) == "\"a\\\"b\"", "AC4: shared form is escaped");
    if (trim_nl(tw) != trim_nl(jit))
        std::println(stderr, "  tw='{}' jit='{}'", tw, jit);
}

// Issue #4468: vectors print elements. Floats use the shortest spelling
// that reads back as the same double, in display, write, and number->string.
static const char* k4468Repro = "(define v (vector 1 2 3))\n"
                                "(display v) (newline)\n"
                                "(write v) (newline)\n"
                                "(display (list 1 v)) (newline)\n"
                                "(display (make-vector 2 0.5)) (newline)\n"
                                "(define x 0.1234567891)\n"
                                "(display x) (newline)\n"
                                "(write x) (newline)\n"
                                "(display (number->string x)) (newline)\n"
                                "(display 123456789.25) (newline)\n"
                                "(display (list 0.0001220703125 2.5)) (newline)\n";

static const char* k4468Expected = "#(1 2 3)\n"
                                   "#(1 2 3)\n"
                                   "(1 #(1 2 3))\n"
                                   "#(0.5 0.5)\n"
                                   "0.1234567891\n"
                                   "0.1234567891\n"
                                   "0.1234567891\n"
                                   "123456789.25\n"
                                   "(0.0001220703125 2.5)\n";

struct Cli4468 {
    int rc = -1;
    std::string out;
    std::string err;
};

static std::string aura_binary_4468() {
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

static Cli4468 run_file_4468(const std::string& bin, const std::string& src,
                             const std::string& assignments) {
    Cli4468 r;
    const char* outp = "/tmp/aura-4468-out.txt";
    const char* errp = "/tmp/aura-4468-err.txt";
    std::string cmd = "timeout 15 env -u AURA_SANDBOX -u AURA_PIPELINE_STRICT -u AURA_PATH " +
                      assignments + " '" + bin + "' '" + src + "' >'" + std::string(outp) +
                      "' 2>'" + std::string(errp) + "'";
    const int raw = std::system(cmd.c_str());
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

static std::string heap_string(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    const auto idx = as_string_idx(v);
    const auto heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return {};
    return std::string(heap[idx]);
}

static void ac4468_vector_and_float_print() {
    std::println("\n--- #4468: vector elements and round-trip floats ---");
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    const char* prev_st = std::getenv("AURA_PIPELINE_STRICT");
    const std::string saved_sb = prev_sb ? prev_sb : "";
    const std::string saved_st = prev_st ? prev_st : "";
    const bool had_sb = prev_sb != nullptr;
    const bool had_st = prev_st != nullptr;
    const auto prev_policy = aura::compiler::tree_walker_fallback_policy();
    struct RestoreFace {
        std::string sb;
        std::string st;
        bool had_sb;
        bool had_st;
        aura::compiler::TreeWalkerFallbackPolicy policy;
        ~RestoreFace() {
            if (had_sb)
                setenv("AURA_SANDBOX", sb.c_str(), 1);
            else
                unsetenv("AURA_SANDBOX");
            if (had_st)
                setenv("AURA_PIPELINE_STRICT", st.c_str(), 1);
            else
                unsetenv("AURA_PIPELINE_STRICT");
            aura::compiler::set_tree_walker_fallback_policy(policy);
        }
    } restore{saved_sb, saved_st, had_sb, had_st, prev_policy};

    auto face = [](const char* strict) {
        setenv("AURA_SANDBOX", "off", 1);
        if (strict && *strict)
            setenv("AURA_PIPELINE_STRICT", strict, 1);
        else
            unsetenv("AURA_PIPELINE_STRICT");
        aura::compiler::apply_pipeline_strict_defaults(true);
    };

    const char* samples[] = {"0.1", "0.0001220703125", "1e-9", "123456789.25", "(/ 1.0 3.0)"};
    const char* faces[] = {"0", "force-soa"};
    for (const char* strict : faces) {
        face(strict);
        const std::string tag = std::string("4468 ") + strict;
        CompilerService cs;
        const std::string printed = with_stdout_capture([&] { (void)cs.eval(k4468Repro); });
        if (printed != k4468Expected)
            std::println(stderr, "{} repro [{}]", tag, printed);
        CHECK(printed == k4468Expected, (tag + " repro").c_str());

        const std::string from_proc = with_stdout_capture([&] {
            (void)cs.eval("(define (show) (display (vector 1 2 3)) (newline) "
                          "(display 0.1234567891) (newline)) (show)");
        });
        CHECK(from_proc == "#(1 2 3)\n0.1234567891\n", (tag + " procedure").c_str());

        auto n42 = cs.eval("(number->string 42)");
        CHECK(n42 && is_string(*n42) && heap_string(cs, *n42) == "42", (tag + " int").c_str());
        auto two = cs.eval("(number->string 2.0)");
        CHECK(two && is_string(*two) && heap_string(cs, *two) == "2.0", (tag + " 2.0").c_str());
        auto tiny = cs.eval("(number->string 1e-9)");
        CHECK(tiny && is_string(*tiny) && heap_string(cs, *tiny) == "1e-09",
              (tag + " 1e-9").c_str());
        auto fmt = cs.eval("(format \"~a\" (vector 1 (vector 2)))");
        CHECK(fmt && is_string(*fmt) && heap_string(cs, *fmt) == "#(1 #(2))",
              (tag + " format").c_str());

        for (const char* sample : samples) {
            const std::string expr(sample);
            auto orig = cs.eval(expr);
            auto back = cs.eval("(string->number (number->string " + expr + "))");
            const bool types_ok = orig && back && is_float(*orig) && is_float(*back);
            double a = 0;
            double b = 0;
            if (types_ok) {
                a = as_float(*orig);
                b = as_float(*back);
            }
            const bool num_ok = types_ok && std::memcmp(&a, &b, sizeof(double)) == 0;
            if (!num_ok)
                std::println(stderr, "{} number->string {}", tag, sample);
            CHECK(num_ok, (tag + " number->string " + expr).c_str());

            const std::string written =
                trim_nl(with_stdout_capture([&] { (void)cs.eval("(write " + expr + ")"); }));
            auto read_back = cs.eval("(string->number \"" + written + "\")");
            double c = 0;
            const bool write_types = orig && read_back && is_float(*orig) && is_float(*read_back);
            if (write_types) {
                a = as_float(*orig);
                c = as_float(*read_back);
            }
            const bool write_ok = write_types && std::memcmp(&a, &c, sizeof(double)) == 0;
            if (!write_ok)
                std::println(stderr, "{} write [{}]", tag, written);
            CHECK(write_ok, (tag + " write " + expr).c_str());
        }
    }

    const auto bin = aura_binary_4468();
    CHECK(!bin.empty(), "4468: aura binary");
    if (bin.empty())
        return;
    const char* path = "/tmp/aura-4468-repro.aura";
    {
        std::ofstream f(path);
        f << k4468Repro;
    }
    const char* file_faces[] = {"AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "", "AURA_SANDBOX=off",
                                "AURA_SANDBOX=off AURA_PIPELINE_STRICT=force-soa"};
    const char* file_tags[] = {"soft", "default", "sandbox", "soa"};
    for (int i = 0; i < 4; ++i) {
        auto run = run_file_4468(bin, path, file_faces[i]);
        if (run.rc != 0 || run.out != k4468Expected || run.out.find("<vector") != std::string::npos)
            std::println(stderr, "4468 {} rc {} out [{}] err [{}]", file_tags[i], run.rc, run.out,
                         run.err);
        CHECK(run.rc == 0, std::string("4468 file ") + file_tags[i]);
        CHECK(run.out == k4468Expected, std::string("4468 file out ") + file_tags[i]);
    }
}

static void ac5_source_gate() {
    std::println("\n--- #2574 AC5: source-cite + gate ---");
    const auto jit = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(jit.find("#2574") != std::string::npos, "AC5: jit runtime cites #2574");
    CHECK(jit.find("fputs_scheme_write_string") != std::string::npos, "AC5: escape helper");
    const auto tw = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    CHECK(tw.find("#2574") != std::string::npos, "AC5: TW io_print_val cites #2574");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_write_string_escape") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2574.json").empty(),
          "AC5: check script (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2574.json").empty(),
          "AC5: gate cmd (manifest SSOT)");
}

} // namespace

int run_test_write_string_escape() {
    std::println("=== Issue #2574: Scheme write string escape ===");
    ac1_write_embedded_quote();
    ac2_write_backslash();
    ac3_display_raw();
    ac4_tw_matches_jit();
    ac4468_vector_and_float_print();
    ac5_source_gate();
    std::println("\n=== #2574: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_write_string_escape();
}
#endif
