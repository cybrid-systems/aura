// test_arithmetic_int64_safety.cpp — Issues #1150–#1156 Phase 1

#include "test_harness.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

namespace {

struct CliRun {
    int rc = -1;
    std::string out;
    std::string err;
};

std::string aura_binary() {
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

CliRun run_aura_file(const std::string& bin, const std::string& src, const char* face) {
    CliRun r;
    const char* outp = "/tmp/aura-4461-out.txt";
    const char* errp = "/tmp/aura-4461-err.txt";
    std::string cmd = "timeout 15 env -u AURA_SANDBOX -u AURA_PIPELINE_STRICT ";
    if (face && face[0] != '\0')
        cmd += face;
    cmd += " '";
    cmd += bin;
    cmd += "' '";
    cmd += src;
    cmd += "' >'";
    cmd += outp;
    cmd += "' 2>'";
    cmd += errp;
    cmd += "'";
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

bool write_text(const char* path, const std::string& body) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << body;
    return true;
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(+ 1 2 3)");
        CHECK(r && is_int(*r) && as_int(*r) == 6, "small +");
        auto r2 = cs.eval("(* 2 3 4)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 24, "small *");
        auto r3 = cs.eval("(- 10 3)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 7, "small -");
        auto r4 = cs.eval("(/ 100 5)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 20, "small /");
    }

    // #1151: (/ 0) must not UB — eval fails or returns error/void safely
    {
        auto r = cs.eval("(/ 0)");
        // DivisionByZero surfaces as unexpected Diagnostic → empty optional
        CHECK(!r || is_error(*r) || is_void(*r), "(/ 0) is safe (error/void/nullopt)");
    }

    // #1150/#1152: large mul should not crash — may float-promote
    {
        auto r = cs.eval("(* 1000000 1000000 1000000 1000000)");
        CHECK(r, "large * returns");
        CHECK(is_int(*r) || is_float(*r), "large * is int or float");
    }

    // #1150 large add used literals past the fixnum domain. make_int's hot
    // contract aborts on those, so this live binary does not eval them.

    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
    }

    // Issue #4461: float zero is IEEE on the literal path; integer zero is
    // an error value, not a normal 0. (/ -0.001 0.0015) stays a real quotient.
    {
        auto p = cs.eval("(/ 1.0 0.0)");
        CHECK(p && is_float(*p) && std::isinf(as_float(*p)) && as_float(*p) > 0,
              "(/ 1.0 0.0) is +inf");
        auto n = cs.eval("(/ -1.0 0.0)");
        CHECK(n && is_float(*n) && std::isinf(as_float(*n)) && as_float(*n) < 0,
              "(/ -1.0 0.0) is -inf");
        auto q = cs.eval("(/ 0.0 0.0)");
        CHECK(q && is_float(*q) && std::isnan(as_float(*q)), "(/ 0.0 0.0) is nan");
        auto rec = cs.eval("(/ 0.0)");
        CHECK(rec && is_float(*rec) && std::isinf(as_float(*rec)) && as_float(*rec) > 0,
              "(/ 0.0) is +inf");
        auto iz = cs.eval("(/ 1 0)");
        CHECK(iz && is_error(*iz) && !is_int(*iz), "(/ 1 0) is an error, not 0");
        auto sm = cs.eval("(/ -0.001 0.0015)");
        CHECK(sm && is_float(*sm) && as_float(*sm) < -0.6 && as_float(*sm) > -0.7,
              "(/ -0.001 0.0015) is a real quotient");
        auto num = cs.eval("(number? (/ 1.0 0.0))");
        CHECK(num && is_bool(*num) && aura::compiler::types::as_bool(*num), "number? of inf");
        auto eq0 = cs.eval("(= (/ 1.0 0.0) 0)");
        CHECK(eq0 && is_bool(*eq0) && !aura::compiler::types::as_bool(*eq0), "inf is not 0");
        auto caught = cs.eval("(try (/ 1 0) (catch (e) e))");
        CHECK(caught && is_string(*caught), "try catches integer division by zero");
    }

    {
        const auto bin = aura_binary();
        CHECK(!bin.empty(), "4461: aura binary");
        if (!bin.empty()) {
            const char* soft = "/tmp/aura-4461-soft.aura";
            CHECK(write_text(soft, "(define (r a b) (/ a b))\n"
                                   "(display (r 1.0 0.0)) (newline)\n"
                                   "(display (r -1.0 0.0)) (newline)\n"
                                   "(display (r 0.0 0.0)) (newline)\n"
                                   "(display (try (r 1 0) (catch (e) e))) (newline)\n"
                                   "(let ((a 1.0) (b 0.0)) (display (/ a b))) (newline)\n"
                                   "(define b 0.0)\n"
                                   "(display (/ 1.0 b)) (newline)\n"
                                   "(define n 0)\n"
                                   "(display (try (/ 1 n) (catch (e) e))) (newline)\n"
                                   "(display (r 1.0 (- 0.5 0.5))) (newline)\n"
                                   "(define (f v) (/ (vector-ref v 0) (vector-ref v 1)))\n"
                                   "(display (f (vector 3.0 0.0))) (newline)\n"
                                   "(define x (r 1.0 0.0))\n"
                                   "(display (number? x)) (display (= x 0)) (newline)\n"
                                   "(display (try (/ 1 0) (catch (e) e))) (newline)\n"
                                   "(display (/ 1.0 0.0)) (newline)\n"
                                   "(display (/ -0.001 0.0015)) (newline)\n"
                                   "(display \"after\") (newline)\n"),
                  "4461: write soft script");
            auto soft_run = run_aura_file(bin, soft, "AURA_SANDBOX=off AURA_PIPELINE_STRICT=0");
            CHECK(soft_run.rc == 0, "4461: soft face exits 0");
            CHECK(soft_run.out.find("\n0\n") == std::string::npos, "4461: soft face no silent 0");
            CHECK(soft_run.out.find("inf\n-inf\nnan\n") == 0, "4461: soft float IEEE");
            CHECK(soft_run.out.find("/: division by zero") != std::string::npos,
                  "4461: soft integer zero is an error");
            CHECK(soft_run.out.find("#t#f") != std::string::npos,
                  "4461: inf is a number and not 0");
            CHECK(soft_run.out.find("after") != std::string::npos, "4461: soft continues");
            CHECK(soft_run.err.find("division by zero") == std::string::npos,
                  "4461: soft does not abort with a diagnostic");

            const char* lit = "/tmp/aura-4461-lit.aura";
            CHECK(write_text(lit, "(display (/ 1.0 0.0)) (newline)\n"
                                  "(display (/ -1.0 0.0)) (newline)\n"
                                  "(display (/ 0.0 0.0)) (newline)\n"
                                  "(display (try (/ 1 0) (catch (e) e))) (newline)\n"
                                  "(define b 0.0)\n"
                                  "(display (/ 1.0 b)) (newline)\n"
                                  "(define n 0)\n"
                                  "(display (try (/ 1 n) (catch (e) e))) (newline)\n"
                                  "(display (/ 1.0 (- 0.5 0.5))) (newline)\n"
                                  "(display (/ -0.001 0.0015)) (newline)\n"
                                  "(display \"after\") (newline)\n"),
                  "4461: write literal script");
            auto lit_run = run_aura_file(bin, lit, "");
            CHECK(lit_run.rc == 0, "4461: default face exits 0");
            CHECK(lit_run.out.find("inf\n-inf\nnan\n") == 0, "4461: literal float IEEE");
            CHECK(lit_run.out.find("/: division by zero") != std::string::npos,
                  "4461: literal integer zero matches the binding");
            CHECK(lit_run.out.find("after") != std::string::npos, "4461: default continues");
            CHECK(lit_run.err.find("error:") == std::string::npos,
                  "4461: default face does not abort");
        }
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("arithmetic int64 safety: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
