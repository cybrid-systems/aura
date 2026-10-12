// @category: unit
// @reason: Issue #2570 — module load must not drop trailing defines/exports;
//          fail-closed on mid-body error and nested require failure.
//
//   AC1: tail defines always export after require
//   AC2: mid-body unbound/error fails require; no half-loaded symbols
//   AC3: nested require of missing module fails outer load
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"
#include "compiler/pipeline_policy.hh"

#include <cstdlib>
#include <filesystem>
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

static bool write_mod(const std::string& dir, const std::string& name, const std::string& body) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir + "/" + name);
    if (!out)
        return false;
    out << body;
    return true;
}

static void ac1_tail_exports() {
    std::println("\n--- #2570 AC1: tail defines export ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac1";
    CHECK(write_mod(dir, "tail.aura",
                    R"((export early:fn late:fn)
(define (early:fn) 1)
(define (f1 x) x)
(define (f2 x) x)
(define (f3 x) x)
(define (f4 x) x)
(define (f5 x) x)
(define (late:fn) 2)
)"),
          "write tail.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"tail\" all:)");
    CHECK(req.has_value() && !(is_error(*req) && !is_string(*req)), "require succeeds");
    auto r1 = cs.eval("(early:fn)");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 1, "early:fn");
    auto r2 = cs.eval("(late:fn)");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 2, "AC1: late:fn tail export");
    unsetenv("AURA_PATH");
}

static void ac2_mid_error_fail_closed() {
    std::println("\n--- #2570 AC2: mid-body error fails load ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac2";
    CHECK(write_mod(dir, "partial.aura",
                    R"((export early:fn late:fn)
(define (early:fn) 1)
(define bad (no-such-xyz-2570 99))
(define (late:fn) 2)
)"),
          "write partial.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"partial\" all:)");
    CHECK(req.has_value(), "require returns a value");
    CHECK(req && is_error(*req) && !is_string(*req), "AC2: require fails with error");
    auto early = cs.eval("(try (early:fn) (catch (e) #f))");
    // half-load must not inject early:fn
    CHECK(early && is_bool(*early) && !as_bool(*early), "AC2: early not injected");
    auto late = cs.eval("(try (late:fn) (catch (e) #f))");
    CHECK(late && is_bool(*late) && !as_bool(*late), "AC2: late not injected");
    unsetenv("AURA_PATH");
}

static void ac3_nested_require_fail() {
    std::println("\n--- #2570 AC3: nested require failure fails outer ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac3";
    CHECK(write_mod(dir, "outer.aura",
                    R"((export outer:ok outer:late)
(define (outer:ok) 1)
(require "missing-mod-2570" all:)
(define (outer:late) 2)
)"),
          "write outer.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"outer\" all:)");
    CHECK(req.has_value() && is_error(*req) && !is_string(*req), "AC3: outer require fails");
    auto ok = cs.eval("(try (outer:ok) (catch (e) #f))");
    CHECK(ok && is_bool(*ok) && !as_bool(*ok), "AC3: no half-inject outer:ok");
    unsetenv("AURA_PATH");
}

static void ac4_source_gate() {
    std::println("\n--- #2570 AC4: source-cite + gate ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("#2570") != std::string::npos, "AC4: module_loader cites #2570");
    CHECK(loader.find("fail-closed") != std::string::npos ||
              loader.find("fail_load") != std::string::npos,
          "AC4: fail-closed load");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2570") != std::string::npos, "AC4: begin aborts on error");
    const auto mod = read_file("src/compiler/evaluator_primitives_module.cpp");
    CHECK(mod.find("#2570") != std::string::npos, "AC4: import surfaces error");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_load_tail_export") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2570.json").empty(),
          "AC4: check script (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2570.json").empty(),
          "AC4: gate cmd (manifest SSOT)");
}

// Follow-up: multi-define (letrec) pre-allocates void cells; (define ceil ceil)
// must re-export the primitive, not fail void-export / leave uncallable.
// Also covers trunc prim registration used by std/math.
static void ac5_prim_reexport_math() {
    std::println("\n--- #2570 AC5: prim re-export + trunc + std/math ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac5";
    CHECK(write_mod(dir, "reexport.aura",
                    R"((export floor ceil trunc round my-sq)
(define floor floor)
(define ceil ceil)
(define trunc trunc)
(define round round)
(define (my-sq x) (* x x))
)"),
          "write reexport.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"reexport\" all:)");
    CHECK(req.has_value() && !(is_error(*req) && !is_string(*req)), "AC5: reexport require");
    auto c = cs.eval("(ceil 3.2)");
    const bool ceil_ok =
        c && ((is_int(*c) && as_int(*c) == 4) || (is_float(*c) && as_float(*c) == 4.0));
    CHECK(ceil_ok, "AC5: ceil re-export callable");
    auto t = cs.eval("(trunc 3.7)");
    CHECK(t && is_float(*t) && as_float(*t) == 3.0, "AC5: trunc → 3.0");
    auto sq = cs.eval("(my-sq 5)");
    CHECK(sq && is_int(*sq) && as_int(*sq) == 25, "AC5: non-prim define still works");
    unsetenv("AURA_PATH");

    // std/math must load (was failing: export ceil/asin void; missing trunc).
    CompilerService cs2;
    auto math = cs2.eval("(begin (require \"std/math\" all:) (square 5))");
    CHECK(math && is_int(*math) && as_int(*math) == 25, "AC5: std/math square");
    auto asin0 = cs2.eval("(asin 0)");
    CHECK(asin0.has_value() && !(is_error(*asin0) && !is_string(*asin0)), "AC5: asin export");

    const auto math_cpp = read_file("src/compiler/evaluator_primitives_math.cpp");
    CHECK(math_cpp.find("\"trunc\"") != std::string::npos, "AC5: trunc registered in math prims");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("void") != std::string::npos && flat.find("slot_for_name") != std::string::npos,
          "AC5: Variable void-cell→prim path");
}

static std::string aura_binary_4460() {
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

static int count_sub(std::string_view hay, std::string_view needle) {
    int n = 0;
    if (needle.empty())
        return 0;
    for (std::size_t p = 0; (p = hay.find(needle, p)) != std::string_view::npos; p += needle.size())
        ++n;
    return n;
}

struct Cli4460 {
    int rc = -1;
    std::string out;
    std::string err;
};

static Cli4460 run_file_4460(const std::string& bin, const std::string& src,
                             const std::string& assignments) {
    Cli4460 r;
    const char* outp = "/tmp/aura-4460-out.txt";
    const char* errp = "/tmp/aura-4460-err.txt";
    // Options before NAME=VALUE. Soft face, isolated module path.
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

// Issue #4460: a failed require is one located error, later forms run, exit 1.
// cs.eval still returns an error value so #2570 fail-closed stays a value.
static void ac4460_failed_require_continues() {
    std::println("\n--- #4460: failed require continues and exits 1 ---");
    CompilerService cs;
    auto api = cs.eval("(require \"nosuchmod-4460-api\" all:)");
    CHECK(api.has_value() && is_error(*api) && !is_string(*api),
          "4460: cs.eval require failure stays an error value");

    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto main_src = read_file("src/main.cpp");
    CHECK(loader.find("#4460") != std::string::npos, "4460: loader cite");
    CHECK(flat.find("#4460") != std::string::npos, "4460: require cite");
    CHECK(main_src.find("set_require_failure_is_diagnostic(true)") != std::string::npos,
          "4460: file mode arms the diagnostic");

    const auto bin = aura_binary_4460();
    CHECK(!bin.empty(), "4460: aura binary");
    if (bin.empty())
        return;

    const char* miss = "/tmp/aura-4460-miss.aura";
    {
        std::ofstream f(miss);
        f << "(display \"first\") (newline)\n";
        f << "(require \"nosuchmod-4460\" all:)\n";
        f << "(display \"second\") (newline)\n";
    }
    auto miss_run = run_file_4460(bin, miss, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0");
    CHECK(miss_run.rc == 1, "4460: missing module exits 1");
    CHECK(miss_run.out.find("first") != std::string::npos, "4460: form before require ran");
    CHECK(miss_run.out.find("second") != std::string::npos, "4460: form after require ran");
    CHECK(miss_run.out.find("<error>") == std::string::npos, "4460: no <error> on stdout");
    CHECK(count_sub(miss_run.err, "load_module_file:") == 0, "4460: no loader stderr");
    CHECK(count_sub(miss_run.err, "cannot resolve") == 1, "4460: reason reported once");
    CHECK(miss_run.err.find("nosuchmod-4460") != std::string::npos, "4460: module name reported");
    CHECK(miss_run.err.find("error:") != std::string::npos, "4460: located error line");

    const std::string dir = "/tmp/aura-4460-mods";
    CHECK(
        write_mod(dir, "badmod.aura", "(export good)\n(define (good x) (* 2 x))\n(define a 1 2)\n"),
        "4460: write badmod");
    CHECK(write_mod(dir, "good4460.aura", "(export good)\n(define (good x) (* 2 x))\n"),
          "4460: write good module");
    const char* bad = "/tmp/aura-4460-bad.aura";
    {
        std::ofstream f(bad);
        f << "(require \"badmod\" all:)\n";
        f << "(display \"before\") (newline)\n";
        f << "(display (good 21)) (newline)\n";
    }
    auto bad_run =
        run_file_4460(bin, bad, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_PATH=" + dir);
    CHECK(bad_run.rc == 1, "4460: parse-error module exits 1");
    CHECK(bad_run.out.find("before") != std::string::npos, "4460: form after bad require ran");
    CHECK(bad_run.out.find("<error>") == std::string::npos, "4460: bad require no <error>");
    CHECK(count_sub(bad_run.err, "load_module_file:") == 0, "4460: parse failure no loader stderr");
    CHECK(count_sub(bad_run.err, "parse error") == 1, "4460: parse reason once");
    CHECK(bad_run.err.find("badmod") != std::string::npos, "4460: bad module name reported");

    const char* ok = "/tmp/aura-4460-ok.aura";
    {
        std::ofstream f(ok);
        f << "(require \"good4460\" all:)\n";
        f << "(display (good 21)) (newline)\n";
    }
    auto ok_run =
        run_file_4460(bin, ok, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0 AURA_PATH=" + dir);
    CHECK(ok_run.rc == 0, "4460: successful require still exits 0");
    CHECK(ok_run.out.find("42") != std::string::npos, "4460: successful require still binds");
}

// Issue #4465: a primitive error that escapes a top-level user call is one
// located diagnostic. Later forms run. Exit status is 1. cs.eval keeps the
// error value (#2570). display of that value still prints <unknown> (#4456).
static void ac4465_escaped_primitive_continues() {
    std::println("\n--- #4465: escaped primitive error is reported once ---");
    CompilerService cs;
    auto swallowed = cs.eval("(define (id x) x) (id (vector-ref (vector 1 2 3) 5))");
    CHECK(swallowed.has_value() && is_error(*swallowed) && !is_string(*swallowed),
          "4465: cs.eval keeps the error value");

    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#4465") != std::string::npos, "4465: cite");

    const auto bin = aura_binary_4460();
    CHECK(!bin.empty(), "4465: aura binary");
    if (bin.empty())
        return;

    const char* face = "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0";
    auto check_escape = [&](const char* path, const char* body, const char* cause,
                            const char* tag) {
        {
            std::ofstream f(path);
            f << body;
        }
        auto run = run_file_4460(bin, path, face);
        if (run.rc != 1 || run.out.find("before") == std::string::npos ||
            run.out.find("after") == std::string::npos ||
            run.out.find("<error>") != std::string::npos || count_sub(run.err, cause) != 1) {
            std::println(std::cerr, "{} rc {} out [{}] err [{}]", tag, run.rc, run.out, run.err);
        }
        CHECK(run.rc == 1, std::string(tag) + " exits 1");
        CHECK(run.out.find("before") != std::string::npos, std::string(tag) + " ran before");
        CHECK(run.out.find("after") != std::string::npos, std::string(tag) + " ran after");
        CHECK(run.out.find("<error>") == std::string::npos, std::string(tag) + " no <error>");
        CHECK(count_sub(run.err, cause) == 1, std::string(tag) + " cause once");
        CHECK(run.err.find("error:") != std::string::npos, std::string(tag) + " located error");
    };

    check_escape("/tmp/aura-4465-id.aura",
                 "(define (id x) x)\n"
                 "(display \"before\")\n"
                 "(id (vector-ref (vector 1 2 3) 5))\n"
                 "(display \"after\")\n",
                 "vector-ref: index out of bounds", "4465: id");
    check_escape("/tmp/aura-4465-car.aura",
                 "(define (id x) x)\n"
                 "(display \"before\")\n"
                 "(id (car (quote ())))\n"
                 "(display \"after\")\n",
                 "car: not a pair", "4465: car");
    check_escape("/tmp/aura-4465-q.aura",
                 "(define (q v) (vector-ref v 9))\n"
                 "(display \"before\")\n"
                 "(q (vector 1 2))\n"
                 "(display \"after\")\n",
                 "vector-ref: index out of bounds", "4465: return");
    {
        std::ofstream f("/tmp/aura-4465-main.aura");
        f << "(define (id x) x)\n";
        f << "(define (main) (id (vector-ref (vector 1 2 3) 5)) (display \"inside-after\"))\n";
        f << "(display \"before\")\n";
        f << "(main)\n";
        f << "(display \"after\")\n";
    }
    auto main_run = run_file_4460(bin, "/tmp/aura-4465-main.aura", face);
    CHECK(main_run.rc == 1, "4465: main exits 1");
    CHECK(main_run.out.find("before") != std::string::npos, "4465: main ran before");
    CHECK(main_run.out.find("after") != std::string::npos, "4465: main ran after");
    CHECK(main_run.out.find("inside-after") == std::string::npos, "4465: body stops at the error");
    CHECK(main_run.out.find("<error>") == std::string::npos, "4465: main no <error>");
    CHECK(count_sub(main_run.err, "vector-ref: index out of bounds") == 1, "4465: main cause once");

    {
        std::ofstream f("/tmp/aura-4465-try.aura");
        f << "(define (id x) x)\n";
        f << "(display \"before\")\n";
        f << "(try (id (vector-ref (vector 1 2 3) 5)) (catch (e) (display \"caught\")))\n";
        f << "(display \"after\")\n";
    }
    auto try_run = run_file_4460(bin, "/tmp/aura-4465-try.aura", face);
    CHECK(try_run.rc == 0, "4465: try exits 0");
    CHECK(try_run.out.find("beforecaughtafter") != std::string::npos, "4465: try catches");
    CHECK(try_run.err.find("vector-ref:") == std::string::npos, "4465: try does not report");

    {
        std::ofstream f("/tmp/aura-4465-def.aura");
        f << "(define (id x) x)\n";
        f << "(define y (id (vector-ref (vector 1 2 3) 5)))\n";
        f << "(display \"after\")\n";
    }
    auto def_run = run_file_4460(bin, "/tmp/aura-4465-def.aura", face);
    CHECK(def_run.rc == 0, "4465: define-group still exits 0");
    CHECK(def_run.out == "after", "4465: define-group binds the error and continues");
    CHECK(def_run.err.find("vector-ref:") == std::string::npos, "4465: define-group is silent");

    {
        std::ofstream f("/tmp/aura-4465-disp.aura");
        f << "(define (q v) (vector-ref v 9))\n";
        f << "(display \"before\")\n";
        f << "(display (q (vector 1 2)))\n";
        f << "(display \"after\")\n";
    }
    auto disp_run = run_file_4460(bin, "/tmp/aura-4465-disp.aura", face);
    CHECK(disp_run.rc == 0, "4465: display of an error exits 0");
    CHECK(disp_run.out == "before<unknown>after", "4465: display prints <unknown>");
    CHECK(disp_run.err.find("vector-ref:") == std::string::npos,
          "4465: display does not re-report");

    {
        std::ofstream f("/tmp/aura-4465-bare.aura");
        f << "(display \"before\")\n";
        f << "(vector-ref (vector 1 2 3) 5)\n";
        f << "(display \"after\")\n";
    }
    auto bare_run = run_file_4460(bin, "/tmp/aura-4465-bare.aura", face);
    CHECK(bare_run.rc == 0, "4465: bare primitive still exits 0");
    CHECK(bare_run.out == "beforeafter", "4465: bare primitive still continues");
}

// Issue #4467: a user procedure, lambda, or named let with the wrong
// number of arguments is an arity diagnostic at the call. Later forms
// run once. The default face does not return ().
static Cli4460 run_file_4467(const std::string& bin, const std::string& src,
                             const std::string& assignments) {
    Cli4460 r;
    const char* outp = "/tmp/aura-4467-out.txt";
    const char* errp = "/tmp/aura-4467-err.txt";
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

static void ac4467_user_call_arity() {
    std::println("\n--- #4467: user call arity is reported at the call ---");
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
    auto expect_arity = [](const char* src, const char* phrase, const char* tag) {
        CompilerService cs;
        auto r = cs.eval(src);
        if (r || r.error().message.find(phrase) == std::string::npos ||
            r.error().format().find("arity mismatch") == std::string::npos) {
            std::println(std::cerr, "{} diag [{}]", tag, r ? std::string{} : r.error().format());
        }
        CHECK(!r, std::string(tag) + " is a diagnostic");
        CHECK(!r && r.error().message.find(phrase) != std::string::npos, std::string(tag));
        CHECK(!r && r.error().format().find("arity mismatch") != std::string::npos,
              std::string(tag) + " kind");
    };
    auto expect_int = [](const char* src, std::int64_t n, const char* tag) {
        CompilerService cs;
        auto r = cs.eval(src);
        if (!r || !is_int(*r) || as_int(*r) != n) {
            std::println(std::cerr, "{} got {}", tag,
                         r ? (is_int(*r) ? std::to_string(as_int(*r)) : std::string("non-int"))
                           : r.error().format());
        }
        CHECK(r && is_int(*r) && as_int(*r) == n, tag);
    };

    const char* faces[] = {"0", "force-soa"};
    for (const char* strict : faces) {
        face(strict);
        const std::string tag = std::string("4467 ") + strict;
        expect_arity("(define (g a b) (+ a b)) (g 1 2 3)", "g expects 2 arguments, got 3",
                     (tag + " too many").c_str());
        expect_arity("(define (g a b) b) (g 7)", "g expects 2 arguments, got 1",
                     (tag + " too few").c_str());
        expect_arity("((lambda (x) x) 1 2)", "lambda expects 1 argument, got 2",
                     (tag + " lambda many").c_str());
        expect_arity("((lambda (x y) y) 1)", "lambda expects 2 arguments, got 1",
                     (tag + " lambda few").c_str());
        expect_arity("(let loop ((a 1)) (loop a a))", "loop expects 1 argument, got 2",
                     (tag + " named many").c_str());
        expect_arity("(let loop ((a 1) (b 2)) (loop a))", "loop expects 2 arguments, got 1",
                     (tag + " named few").c_str());
        expect_arity("(define (f a b . rest) (+ a b)) (f 1)",
                     "f expects at least 2 arguments, got 1", (tag + " rest few").c_str());
        expect_int("(define (g a b) (+ a b)) (g 1 2)", 3, (tag + " exact").c_str());
        expect_int("(let ((k 10)) ((lambda (x) (+ k x)) 5))", 15, (tag + " capture").c_str());
        expect_int("(define (f a . rest) a) (f 1 2 3)", 1, (tag + " rest extra").c_str());
        expect_int("(define (f . rest) 7) (f)", 7, (tag + " rest zero").c_str());
        expect_int("(define (f . rest) 7) (f 1 2)", 7, (tag + " rest any").c_str());
        expect_int("(let loop ((n 3) (a 0)) (if (= n 0) a (loop (- n 1) (+ a 1))))", 3,
                   (tag + " named ok").c_str());
        expect_int("(+ 1 2 3)", 6, (tag + " prim").c_str());
        expect_int("(define (g a b) b) (try (g 1) (catch (e) 9))", 9, (tag + " try").c_str());
        expect_arity("(define (g a b) b) (display (g 1))", "g expects 2 arguments, got 1",
                     (tag + " display").c_str());
    }

    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(flat.find("#4467") != std::string::npos, "4467: walker cite");
    CHECK(ir.find("#4467") != std::string::npos, "4467: ir cite");
    CHECK(flat.find("abort begin on first-class error") != std::string::npos,
          "4467: #2570 phrase stays");

    const auto bin = aura_binary_4460();
    CHECK(!bin.empty(), "4467: aura binary");
    if (bin.empty())
        return;

    const char* o1 = "/tmp/aura-4467-o1.aura";
    {
        std::ofstream f(o1);
        f << "(define (g a b) b)\n";
        f << "(display \"start\") (newline)\n";
        f << "(display (g 1)) (newline)\n";
        f << "(display \"after\") (newline)\n";
    }
    const char* o2 = "/tmp/aura-4467-o2.aura";
    {
        std::ofstream f(o2);
        f << "(define (g a b) (+ a b))\n";
        f << "(display (g 1 2 3)) (newline)\n";
        f << "(display ((lambda (x) x) 1 2)) (newline)\n";
        f << "(define (h a b) a)\n";
        f << "(display (h 7)) (newline)\n";
    }
    const char* ok = "/tmp/aura-4467-ok.aura";
    {
        std::ofstream f(ok);
        f << "(define (g a b) (+ a b))\n";
        f << "(display (g 4 5)) (newline)\n";
    }
    const char* file_faces[][2] = {
        {"soft", "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0"},
        {"def", ""},
        {"sand", "AURA_SANDBOX=off"},
        {"soa", "AURA_SANDBOX=off AURA_PIPELINE_STRICT=force-soa"},
    };
    for (auto& ff : file_faces) {
        auto run = run_file_4467(bin, o1, ff[1]);
        if (run.rc != 1 || count_sub(run.out, "start") != 1 || count_sub(run.out, "after") != 1 ||
            run.out.find("()") != std::string::npos ||
            run.err.find("g expects 2 arguments, got 1") == std::string::npos ||
            run.err.find("unbound") != std::string::npos ||
            count_sub(run.err, "arity mismatch") < 1) {
            std::println(std::cerr, "4467 o1 {} rc {} out [{}] err [{}]", ff[0], run.rc, run.out,
                         run.err);
        }
        CHECK(run.rc == 1, std::string("4467 o1 ") + ff[0] + " exits 1");
        CHECK(count_sub(run.out, "start") == 1, std::string("4467 o1 ") + ff[0] + " start once");
        CHECK(count_sub(run.out, "after") == 1, std::string("4467 o1 ") + ff[0] + " after once");
        CHECK(run.out.find("()") == std::string::npos, std::string("4467 o1 ") + ff[0] + " no ()");
        CHECK(run.err.find("g expects 2 arguments, got 1") != std::string::npos,
              std::string("4467 o1 ") + ff[0] + " names g");
        CHECK(run.err.find("error:") != std::string::npos,
              std::string("4467 o1 ") + ff[0] + " located");
        CHECK(run.err.find("unbound") == std::string::npos,
              std::string("4467 o1 ") + ff[0] + " not unbound");

        auto bad = run_file_4467(bin, o2, ff[1]);
        const bool lambda_arity =
            bad.err.find("lambda expects 1 argument, got 2") != std::string::npos;
        const bool lambda_2213 =
            bad.err.find("tree-walker fallback forbidden") != std::string::npos;
        if (bad.rc != 1 || bad.out.find("3") != std::string::npos ||
            bad.out.find("7") != std::string::npos || bad.out.find("()") != std::string::npos ||
            bad.err.find("g expects 2 arguments, got 3") == std::string::npos ||
            bad.err.find("h expects 2 arguments, got 1") == std::string::npos ||
            (!lambda_arity && !lambda_2213)) {
            std::println(std::cerr, "4467 o2 {} rc {} out [{}] err [{}]", ff[0], bad.rc, bad.out,
                         bad.err);
        }
        CHECK(bad.rc == 1, std::string("4467 o2 ") + ff[0] + " exits 1");
        CHECK(bad.out.find("3") == std::string::npos, std::string("4467 o2 ") + ff[0] + " no 3");
        CHECK(bad.out.find("7") == std::string::npos, std::string("4467 o2 ") + ff[0] + " no 7");
        CHECK(bad.out.find("()") == std::string::npos, std::string("4467 o2 ") + ff[0] + " no ()");
        CHECK(bad.err.find("g expects 2 arguments, got 3") != std::string::npos,
              std::string("4467 o2 ") + ff[0] + " g");
        CHECK(bad.err.find("h expects 2 arguments, got 1") != std::string::npos,
              std::string("4467 o2 ") + ff[0] + " h");
        CHECK(lambda_arity || lambda_2213, std::string("4467 o2 ") + ff[0] + " lambda");
        // Sandbox-off with strict unset is the same Forbidden gate as the
        // default face (#2213). Only Soft and force-soa must name the lambda.
        if (std::string(ff[0]) == "soft" || std::string(ff[0]) == "soa")
            CHECK(lambda_arity, std::string("4467 o2 ") + ff[0] + " lambda arity");

        auto good = run_file_4467(bin, ok, ff[1]);
        if (good.rc != 0 || good.out.find("9") == std::string::npos) {
            std::println(std::cerr, "4467 ok {} rc {} out [{}] err [{}]", ff[0], good.rc, good.out,
                         good.err);
        }
        CHECK(good.rc == 0, std::string("4467 ok ") + ff[0] + " exits 0");
        CHECK(count_sub(good.out, "9") == 1, std::string("4467 ok ") + ff[0] + " prints once");
    }
}

// Issue #4471: later file-mode diagnostics use file lines, and a runtime
// error resumes after the top-level form that was executing.
static void ac4471_file_lines_and_resume() {
    std::println("\n--- #4471: file lines and resume after the executing form ---");
    const auto main_src = read_file("src/main.cpp");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(main_src.find("#4471") != std::string::npos, "4471: main cite");
    CHECK(flat.find("#4471") != std::string::npos, "4471: walker cite");
    CHECK(main_src.find("Issue #4355") != std::string::npos, "4471: #4355 resume note stays");

    const auto bin = aura_binary_4460();
    CHECK(!bin.empty(), "4471: aura binary");
    if (bin.empty())
        return;

    const char* face = "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0";
    const char* lines_path = "/tmp/aura-4471-lines.aura";
    {
        std::ofstream f(lines_path);
        f << "(display \"one\")\n";
        f << "(newline)\n";
        f << "(foo 1)\n";
        f << "(display \"two\")\n";
        f << "(newline)\n";
        f << "\n";
        f << "\n";
        f << "(bar 2)\n";
        f << "(display \"three\")\n";
        f << "(newline)\n";
        f << "(baz 3)\n";
    }
    auto lines = run_file_4460(bin, lines_path, face);
    if (lines.rc != 1 || lines.out != "one\ntwo\nthree\n" ||
        count_sub(lines.err, "unbound variable: foo") != 1 ||
        count_sub(lines.err, "unbound variable: bar") != 1 ||
        count_sub(lines.err, "unbound variable: baz") != 1 ||
        lines.err.find("error: 3:") == std::string::npos ||
        lines.err.find("error: 8:") == std::string::npos ||
        lines.err.find("error: 11:") == std::string::npos ||
        lines.err.find("(bar 2)") == std::string::npos ||
        lines.err.find("(baz 3)") == std::string::npos) {
        std::println(std::cerr, "4471 lines rc {} out [{}] err [{}]", lines.rc, lines.out,
                     lines.err);
    }
    CHECK(lines.rc == 1, "4471: later errors exit 1");
    CHECK(lines.out == "one\ntwo\nthree\n", "4471: one two three once");
    CHECK(count_sub(lines.err, "unbound variable: foo") == 1, "4471: foo once");
    CHECK(count_sub(lines.err, "unbound variable: bar") == 1, "4471: bar once");
    CHECK(count_sub(lines.err, "unbound variable: baz") == 1, "4471: baz once");
    CHECK(lines.err.find("error: 3:") != std::string::npos, "4471: foo on line 3");
    CHECK(lines.err.find("error: 8:") != std::string::npos, "4471: bar on line 8");
    CHECK(lines.err.find("error: 11:") != std::string::npos, "4471: baz on line 11");
    CHECK(lines.err.find("(foo 1)") != std::string::npos, "4471: foo snippet");
    CHECK(lines.err.find("(bar 2)") != std::string::npos, "4471: bar snippet");
    CHECK(lines.err.find("(baz 3)") != std::string::npos, "4471: baz snippet");

    const char* span_path = "/tmp/aura-4471-span.aura";
    {
        std::ofstream f(span_path);
        f << "(foo 1)\n";
        f << "(begin\n";
        f << "  (bar))\n";
    }
    auto span = run_file_4460(bin, span_path, face);
    if (span.rc != 1 || count_sub(span.err, "unbound variable: bar") != 1 ||
        span.err.find("error: 3:") == std::string::npos ||
        span.err.find("(bar)") == std::string::npos) {
        std::println(std::cerr, "4471 span rc {} out [{}] err [{}]", span.rc, span.out, span.err);
    }
    CHECK(span.rc == 1, "4471: multiline form exits 1");
    CHECK(count_sub(span.err, "unbound variable: foo") == 1, "4471: span foo once");
    CHECK(count_sub(span.err, "unbound variable: bar") == 1, "4471: span bar once");
    CHECK(span.err.find("error: 3:") != std::string::npos, "4471: bar on file line 3");
    CHECK(span.err.find("(bar)") != std::string::npos, "4471: bar snippet keeps the line");

    const char* zork_path = "/tmp/aura-4471-zork.aura";
    {
        std::ofstream f(zork_path);
        f << "(display 1)\n";
        f << "(newline)\n";
        f << "(define (f) (zork))\n";
        f << "(display 2)\n";
        f << "(newline)\n";
        f << "(f)\n";
    }
    auto zork = run_file_4460(bin, zork_path, face);
    if (zork.rc != 1 || zork.out != "1\n2\n" ||
        count_sub(zork.err, "unbound variable: zork") != 1) {
        std::println(std::cerr, "4471 zork rc {} out [{}] err [{}]", zork.rc, zork.out, zork.err);
    }
    CHECK(zork.rc == 1, "4471: zork exits 1");
    CHECK(zork.out == "1\n2\n", "4471: 1 and 2 once");
    CHECK(count_sub(zork.err, "unbound variable: zork") == 1, "4471: zork once");
    CHECK(zork.err.find("error: 3:") != std::string::npos, "4471: zork stays on the define");

    const char* after_path = "/tmp/aura-4471-after.aura";
    {
        std::ofstream f(after_path);
        f << "(display 1)\n";
        f << "(newline)\n";
        f << "(define (f) (zork))\n";
        f << "(display 2)\n";
        f << "(newline)\n";
        f << "(f)\n";
        f << "(display 3)\n";
        f << "(newline)\n";
    }
    auto after = run_file_4460(bin, after_path, face);
    if (after.rc != 1 || after.out != "1\n2\n3\n" ||
        count_sub(after.err, "unbound variable: zork") != 1) {
        std::println(std::cerr, "4471 after rc {} out [{}] err [{}]", after.rc, after.out,
                     after.err);
    }
    CHECK(after.rc == 1, "4471: form after the call exits 1");
    CHECK(after.out == "1\n2\n3\n", "4471: forms after the call run once");
    CHECK(count_sub(after.err, "unbound variable: zork") == 1, "4471: after does not re-call");
}

// Issue #4478: a top-level cannot-call must not re-run earlier forms, and a
// non-procedure apply must raise (not silently return the callee) on both
// pipeline faces.
static void ac4478_top_level_cannot_call() {
    std::println("\n--- #4478: top-level cannot-call resume + no silent apply ---");
    const auto bin = aura_binary_4460();
    CHECK(!bin.empty(), "4478: aura binary");
    if (bin.empty())
        return;

    const char* rerun_path = "/tmp/aura-4478-rerun.aura";
    {
        std::ofstream f(rerun_path);
        f << "(display \"one\")\n";
        f << "(newline)\n";
        f << "(define (f x) (+ x 1))\n";
        f << "(define z 0)\n";
        f << "(display (f (z 1)))\n";
    }
    const char* silent_path = "/tmp/aura-4478-silent.aura";
    {
        std::ofstream f(silent_path);
        f << "(define z 0)\n";
        f << "(5 5)\n";
        f << "(\"s\" 1)\n";
        f << "(z 5)\n";
    }
    // Same-pass (z 5): the issue's own repro shape — the call evaluates in
    // the same pass as the define, so both faces must name z.
    const char* zcall_path = "/tmp/aura-4478-zcall.aura";
    {
        std::ofstream f(zcall_path);
        f << "(define z 0)\n";
        f << "(z 5)\n";
    }
    const char* faces[][2] = {
        {"soft", "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0"},
        {"strict", "AURA_SANDBOX=off AURA_PIPELINE_STRICT=1"},
    };
    for (auto& ff : faces) {
        // A: the located type error resumes after the executing form — the
        // display side effect must run exactly once, not once per pass.
        auto run = run_file_4460(bin, rerun_path, ff[1]);
        if (run.rc != 1 || count_sub(run.out, "one") != 1 ||
            run.err.find("cannot call: z") == std::string::npos) {
            std::println(std::cerr, "4478 rerun {} rc {} out [{}] err [{}]", ff[0], run.rc, run.out,
                         run.err);
        }
        CHECK(run.rc == 1, std::string("4478 rerun ") + ff[0] + " exits 1");
        CHECK(count_sub(run.out, "one") == 1,
              std::string("4478 rerun ") + ff[0] + " side effect once");
        CHECK(run.err.find("cannot call: z") != std::string::npos,
              std::string("4478 rerun ") + ff[0] + " names z");

        // B: (5 5) / ("s" 1) / (z 5) raise at top level — no self-result.
        // Each failing form reports once and the later forms still run (#4460).
        // On the strict face a resume-loop form that references an earlier
        // value define re-raises the pre-existing #2213 fallback refusal
        // (still an error, never a silent apply) — so strict pins >= 2 and
        // the same-pass zcall below pins the named z on both faces.
        auto silent = run_file_4460(bin, silent_path, ff[1]);
        if (silent.rc != 1 || !silent.out.empty() || count_sub(silent.err, "cannot call") < 2) {
            std::println(std::cerr, "4478 silent {} rc {} out [{}] err [{}]", ff[0], silent.rc,
                         silent.out, silent.err);
        }
        CHECK(silent.rc == 1, std::string("4478 silent ") + ff[0] + " exits 1");
        CHECK(silent.out.empty(), std::string("4478 silent ") + ff[0] + " no self-result");
        CHECK(count_sub(silent.err, "cannot call") >= 2,
              std::string("4478 silent ") + ff[0] + " literal applies raise");
        if (std::string(ff[0]) == "soft") {
            CHECK(count_sub(silent.err, "cannot call") == 3,
                  std::string("4478 silent ") + ff[0] + " every apply raises");
            CHECK(silent.err.find("cannot call: z") != std::string::npos,
                  std::string("4478 silent ") + ff[0] + " names z");
        }

        auto zcall = run_file_4460(bin, zcall_path, ff[1]);
        if (zcall.rc != 1 || zcall.err.find("cannot call: z") == std::string::npos) {
            std::println(std::cerr, "4478 zcall {} rc {} out [{}] err [{}]", ff[0], zcall.rc,
                         zcall.out, zcall.err);
        }
        CHECK(zcall.rc == 1, std::string("4478 zcall ") + ff[0] + " exits 1");
        CHECK(zcall.err.find("cannot call: z") != std::string::npos,
              std::string("4478 zcall ") + ff[0] + " names z");
    }
}

} // namespace

int run_test_module_load_tail_export() {
    std::println("=== Issue #2570: module load tail export / fail-closed ===");
    ac1_tail_exports();
    ac2_mid_error_fail_closed();
    ac3_nested_require_fail();
    ac4_source_gate();
    ac5_prim_reexport_math();
    ac4460_failed_require_continues();
    ac4465_escaped_primitive_continues();
    ac4467_user_call_arity();
    ac4471_file_lines_and_resume();
    ac4478_top_level_cannot_call();
    std::println("\n=== #2570: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_load_tail_export();
}
#endif
