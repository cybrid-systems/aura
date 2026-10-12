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
using aura::compiler::types::as_bool;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
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

// Issue #4472: code-point indexes, scalar char?, and radix.
static void ac4472_codepoints_and_radix() {
    std::println("\n--- #4472: code points, char?, radix ---");
    const char* prev_sb = std::getenv("AURA_SANDBOX");
    const char* prev_st = std::getenv("AURA_PIPELINE_STRICT");
    const char* prev_path = std::getenv("AURA_PATH");
    const std::string saved_sb = prev_sb ? prev_sb : "";
    const std::string saved_st = prev_st ? prev_st : "";
    const std::string saved_path = prev_path ? prev_path : "";
    const bool had_sb = prev_sb != nullptr;
    const bool had_st = prev_st != nullptr;
    const bool had_path = prev_path != nullptr;
    const auto prev_policy = aura::compiler::tree_walker_fallback_policy();
    struct RestoreFace {
        std::string sb, st, path;
        bool had_sb, had_st, had_path;
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
            if (had_path)
                setenv("AURA_PATH", path.c_str(), 1);
            else
                unsetenv("AURA_PATH");
            aura::compiler::set_tree_walker_fallback_policy(policy);
        }
    } restore{saved_sb, saved_st, saved_path, had_sb, had_st, had_path, prev_policy};
    setenv("AURA_PATH", AURA_SOURCE_DIR "/lib", 1);

    const char* faces[] = {"0", "force-soa"};
    for (const char* strict : faces) {
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", strict, 1);
        aura::compiler::apply_pipeline_strict_defaults(true);
        const std::string tag = std::string("4472 ") + strict;
        CompilerService cs;
        auto ival = [&](const char* src) -> std::int64_t {
            auto r = cs.eval(src);
            return (r && is_int(*r)) ? as_int(*r) : static_cast<std::int64_t>(-999);
        };
        auto sval = [&](const char* src) -> std::string {
            auto r = cs.eval(src);
            return (r && is_string(*r)) ? heap_string(cs, *r) : std::string{};
        };
        auto bval = [&](const char* src) -> int {
            auto r = cs.eval(src);
            if (!r || !is_bool(*r))
                return -1;
            return as_bool(*r) ? 1 : 0;
        };
        auto iserr = [&](const char* src) {
            auto r = cs.eval(src);
            return r && is_error(*r);
        };

        CHECK(ival(R"((string-length "hello"))") == 5, (tag + " hello").c_str());
        CHECK(ival(R"((string-length "h\xC3\xA9llo"))") == 5, (tag + " hello-acute").c_str());
        CHECK(ival(R"((string-length "\xE5\xBC\xBA\xE5\x8C\x96"))") == 2,
              (tag + " two han").c_str());
        CHECK(ival(R"((string-length "\xFF\x80"))") == 2, (tag + " two bad bytes").c_str());
        CHECK(ival(R"((string-length "\xC3"))") == 1, (tag + " truncated lead").c_str());
        CHECK(ival(R"((string-length 12345))") == 0, (tag + " length of int").c_str());
        CHECK(ival(R"((string-ref "h\xC3\xA9llo" 1))") == 233, (tag + " ref acute").c_str());
        CHECK(ival(R"((string-ref "h\xC3\xA9llo" 2))") == 108, (tag + " ref after acute").c_str());
        CHECK(ival(R"((string-ref "\xE5\xBC\xBA\xE5\x8C\x96" 0))") == 0x5F3A,
              (tag + " ref han0").c_str());
        CHECK(ival(R"((string-ref "\xE5\xBC\xBA\xE5\x8C\x96" 1))") == 0x5316,
              (tag + " ref han1").c_str());
        CHECK(ival(R"((string-ref "\xC3" 0))") == 195, (tag + " ref bad lead").c_str());
        CHECK(ival(R"((string-ref "a" 0))") == 97, (tag + " ref ascii").c_str());
        CHECK(iserr(R"((string-ref 12345 0))"), (tag + " ref of int").c_str());
        CHECK(sval(R"((substring "\xE5\xBC\xBA\xE5\x8C\x96" 0 1))") == "\xE5\xBC\xBA",
              (tag + " sub one").c_str());
        CHECK(sval(R"((substring "\xE5\xBC\xBA\xE5\x8C\x96" 0 2))") == "\xE5\xBC\xBA\xE5\x8C\x96",
              (tag + " sub both").c_str());
        CHECK(sval(R"((substring "\xE5\xBC\xBA\xE5\x8C\x96" 1 2))") == "\xE5\x8C\x96",
              (tag + " sub second").c_str());
        CHECK(ival(R"((string-index "\xE5\xBC\xBA\xE5\x8C\x96x" "x"))") == 2,
              (tag + " index").c_str());
        CHECK(ival(R"((car (cdr (string->list "h\xC3\xA9llo"))))") == 233,
              (tag + " list acute").c_str());
        CHECK(bval(R"((equal? (list->string (string->list "h\xC3\xA9llo")) "h\xC3\xA9llo"))") == 1,
              (tag + " round trip").c_str());
        CHECK(bval(R"((char=? #\a (string-ref "a" 0)))") == 1, (tag + " char=?").c_str());
        CHECK(bval(R"((char? #\a))") == 1, (tag + " char? literal").c_str());
        CHECK(bval(R"((char? 5))") == 1, (tag + " char? scalar 5").c_str());
        CHECK(bval(R"((char? -1))") == 0, (tag + " char? neg").c_str());
        CHECK(bval(R"((char? "a"))") == 0, (tag + " char? string").c_str());
        CHECK(bval(R"((char? 55296))") == 0, (tag + " char? surrogate").c_str());
        CHECK(bval(R"((char? 1114111))") == 1, (tag + " char? max").c_str());
        CHECK(bval(R"((char? 1114112))") == 0, (tag + " char? past").c_str());
        CHECK(sval(R"((number->string 255 16))") == "ff", (tag + " hex").c_str());
        CHECK(sval(R"((number->string 10 2))") == "1010", (tag + " bin").c_str());
        CHECK(sval(R"((number->string -10 2))") == "-1010", (tag + " neg bin").c_str());
        CHECK(sval(R"((number->string 255))") == "255", (tag + " dec").c_str());
        CHECK(sval(R"((number->string 1e-9))") == "1e-09", (tag + " tiny").c_str());
        CHECK(sval(R"((number->string 1e-9 10))") == "1e-09", (tag + " tiny radix 10").c_str());
        CHECK(sval(R"((number->string 2.0))") == "2.0", (tag + " 2.0").c_str());
        CHECK(iserr(R"((number->string 2.0 16))"), (tag + " float radix").c_str());
        CHECK(iserr(R"((number->string 255 1))"), (tag + " bad radix").c_str());
        CHECK(ival(R"((string->number "ff" 16))") == 255, (tag + " parse hex").c_str());
        CHECK(ival(R"((string->number "FF" 16))") == 255, (tag + " parse HEX").c_str());
        CHECK(ival(R"((string->number "1010" 2))") == 10, (tag + " parse bin").c_str());
        CHECK(ival(R"((string->number "  ff" 16))") == 255, (tag + " parse trim").c_str());
        CHECK(ival(R"((string->number "+7"))") == 7, (tag + " plus").c_str());
        CHECK(ival(R"((string->number "9007199254740993"))") == 9007199254740993LL,
              (tag + " 2^53+1").c_str());
        CHECK(bval(R"((string->number "ff"))") == 0, (tag + " hex without radix").c_str());
        CHECK(bval(R"((string->number "nope"))") == 0, (tag + " nope").c_str());
        CHECK(iserr(R"((string->number "10" 1))"), (tag + " parse bad radix").c_str());
        auto big = cs.eval(R"((string->number "4611686018427387904"))");
        CHECK(big && is_float(*big) && as_float(*big) == 4611686018427387904.0,
              (tag + " 2^62 float").c_str());

        auto imported = cs.eval(R"((import "std/string"))");
        CHECK(imported.has_value(), (tag + " import").c_str());
        CHECK(sval(R"((string-pad-left "\xE5\xBC\xBA" 4))") == "   \xE5\xBC\xBA",
              (tag + " pad").c_str());
        CHECK(sval(R"((string-upcase "h\xC3\xA9llo"))") == "H\xC3\xA9LLO",
              (tag + " upcase").c_str());
        CHECK(sval(R"((string-reverse "\xE5\xBC\xBA\xE5\x8C\x96"))") == "\xE5\x8C\x96\xE5\xBC\xBA",
              (tag + " reverse").c_str());
    }

    const auto bin = aura_binary_4468();
    CHECK(!bin.empty(), "4472: aura binary");
    if (bin.empty())
        return;
    const char* path = "/tmp/aura-4472-repro.aura";
    {
        std::ofstream f(path);
        f << R"((display (string-length "h\xC3\xA9llo")) (newline)
(display (string-length "\xE5\xBC\xBA\xE5\x8C\x96")) (newline)
(display (string-ref "h\xC3\xA9llo" 1)) (newline)
(display (substring "\xE5\xBC\xBA\xE5\x8C\x96" 0 1)) (newline)
(display (string-length 12345)) (newline)
(display (char? -1)) (newline)
(display (char? "a")) (newline)
(display (number->string 255 16)) (newline)
(display (number->string 10 2)) (newline)
(display (string->number "ff" 16)) (newline)
(import "std/string")
(display (string-pad-left "\xE5\xBC\xBA" 4)) (newline)
(display (string-upcase "h\xC3\xA9llo")) (newline)
)";
    }
    const std::string expect =
        "5\n2\n233\n\xE5\xBC\xBA\n0\n#f\n#f\nff\n1010\n255\n   \xE5\xBC\xBA\nH\xC3\xA9LLO\n";
    auto run = run_file_4468(
        bin, path, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_PATH=" AURA_SOURCE_DIR "/lib");
    if (run.rc != 0 || run.out != expect)
        std::println(stderr, "4472 file rc {} out [{}] err [{}]", run.rc, run.out, run.err);
    CHECK(run.rc == 0, "4472 file rc");
    CHECK(run.out == expect, "4472 file out");
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
    ac4472_codepoints_and_radix();
    ac5_source_gate();
    std::println("\n=== #2574: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_write_string_escape();
}
#endif
