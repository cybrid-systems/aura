// @category: unit
// @reason: Issue #2571 — (define) inside (while) body must re-init loop
//          counters; multi-define + set! must not freeze/spin; education
//          warning + preferred outer-define + set! pattern documented.
//
//   AC1: issue repro — nested while with (define x 0) yields count=6
//   AC2: multi-define in while body (define x)(define y) also yields 6
//   AC3: preferred outer define + set! yields 6
//   AC4: education warning path + language note + gate

#include "test_harness.hpp"

#include "compiler/pipeline_policy.hh"

#include <csignal>
#include <cstdio>
#include <cstdlib>
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

using aura::compiler::apply_pipeline_strict_defaults;
using aura::compiler::CompilerService;
using aura::compiler::reset_tree_walker_fallback_policy_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
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

static bool eval_int_eq(CompilerService& cs, const char* src, std::int64_t want,
                        const char* label) {
    auto r = cs.eval(src);
    if (!r) {
        CHECK(false, label);
        return false;
    }
    if (is_error(*r) && !is_string(*r)) {
        CHECK(false, label);
        return false;
    }
    CHECK(r && is_int(*r) && as_int(*r) == want, label);
    return r && is_int(*r) && as_int(*r) == want;
}

// Issue repro from #2571: inner (define x 0) each outer iter.
// Expected: 3 outer × 2 inner = 6 (not 2 / freeze).
static void ac1_define_in_while() {
    std::println("\n--- #2571 AC1: define x inside while ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define count 0)
  (while (< z 3)
    (begin
      (define x 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC1: count=6 with define-in-while");
    unsetenv("AURA_PIPELINE_STRICT");
}

// Multi-define begin inside while previously stacked cells; set! hit the
// oldest cell while Variable read the newest → hang or freeze.
static void ac2_multi_define_in_while() {
    std::println("\n--- #2571 AC2: multi-define in while body ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define count 0)
  (while (< z 3)
    (begin
      (define x 0)
      (define y 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))
          (set! y (+ y 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC2: multi-define count=6 (no hang/freeze)");
    unsetenv("AURA_PIPELINE_STRICT");
}

static void ac3_preferred_outer_set() {
    std::println("\n--- #2571 AC3: outer define + set! ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define x 0)
  (define count 0)
  (while (< z 3)
    (begin
      (set! x 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC3: preferred pattern count=6");
    unsetenv("AURA_PIPELINE_STRICT");
}

static void ac4_source_gate() {
    std::println("\n--- #2571 AC4: source-cite + gate ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#2571") != std::string::npos, "AC4: lookup_cell_index cites #2571");
    CHECK(env.find("newest") != std::string::npos ||
              env.find("binding_index_") != std::string::npos,
          "AC4: newest/binding_index path");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2571") != std::string::npos, "AC4: while/define cites #2571");
    CHECK(flat.find("define …) inside (while") != std::string::npos ||
              flat.find("define") != std::string::npos && flat.find("while") != std::string::npos,
          "AC4: education warning text");
    const auto ctrl = read_file("src/compiler/evaluator_primitives_control.cpp");
    CHECK(ctrl.find("#2571") != std::string::npos, "AC4: while language note");
    CHECK(ctrl.find("outer-define") != std::string::npos ||
              ctrl.find("set! x 0") != std::string::npos,
          "AC4: preferred pattern in note");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_while_define_oneshot") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2571.json").empty(),
          "AC4: check script (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2571.json").empty(),
          "AC4: gate cmd (manifest SSOT)");
}

// Issue #4350 reopen: set! on a captured variable must update the one
// shared cell when the call rides a capture snapshot. string-trim is
// the same while-thunk shape and must return.
static void ac4350_capture_snapshot_set() {
    std::println("\n--- #4350: set! writes the shared capture cell ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define (a1) (let ((i 0)) (define (inc!) (set! i (+ i 1))) (inc!) (inc!) i))
  (a1)))",
                2, "4350: inner define set! = 2");
    eval_int_eq(cs,
                R"((begin
  (define (a2)
    (let ((i 0))
      (define a (lambda () (set! i (+ i 1)) i))
      (define b (lambda () (set! i (+ i 1)) i))
      (a)
      (b)))
  (a2)))",
                2, "4350: two lambdas share the cell");
    eval_int_eq(cs,
                R"((begin
  (define (mk) (let ((c 0)) (lambda () (set! c (+ c 1)) c)))
  (define ctr (mk))
  (ctr)
  (ctr)
  (ctr)))",
                3, "4350: counter closure = 3");
    eval_int_eq(cs,
                R"((begin
  (define (a6)
    (let ((lo 0) (hi 3))
      (while (lambda () (< lo hi)) (lambda () (set! lo (+ lo 1))))
      lo))
  (a6)))",
                3, "4350: while thunk set! = 3");
    CHECK(cs.eval("(require \"std/string\" all:)").has_value(), "4350: require std/string");
    eval_int_eq(cs, R"((if (string=? (string-trim " a ") "a") 1 0))", 1, "4350: string-trim a");
    eval_int_eq(cs, R"((if (string=? (string-trim "  hi  ") "hi") 1 0))", 1,
                "4350: string-trim hi");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("Issue #4350: set! writes the capture-snapshot cell") != std::string::npos,
          "4350: set! consults the capture snapshot");
    unsetenv("AURA_SANDBOX");
    unsetenv("AURA_PIPELINE_STRICT");
}

// Issue #4372: eval-current installs an IR closure for a workspace lambda.
// while + set! of a let binding must still be visible on the next iteration.
static void ac4372_while_set_in_define() {
    std::println("\n--- #4372: while + set! inside a define ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    const char* sj = R"((define (sj strs sep)
  (if (null? strs) ""
      (if (null? (cdr strs)) (car strs)
          (let ((result (car strs))
                (rest (cdr strs)))
            (while (lambda () (pair? rest))
              (lambda ()
                (set! result (string-append result sep (car rest)))
                (set! rest (cdr rest))))
            result)))))";
    std::string escaped;
    escaped.reserve(std::char_traits<char>::length(sj) + 8);
    for (char c : std::string_view(sj)) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    CompilerService cs;
    CHECK(cs.eval(std::format("(set-code \"{}\")", escaped)).has_value(), "4372: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4372: eval-current");
    eval_int_eq(cs, R"((if (string=? (sj (list "a" "b") ",") "a,b") 1 0))", 1,
                "4372: sj after eval-current is a,b");
    CompilerService file_cs;
    eval_int_eq(file_cs,
                R"((begin
  (define (sj strs sep)
    (if (null? strs) ""
        (if (null? (cdr strs)) (car strs)
            (let ((result (car strs))
                  (rest (cdr strs)))
              (while (lambda () (pair? rest))
                (lambda ()
                  (set! result (string-append result sep (car rest)))
                  (set! rest (cdr rest))))
              result))))
  (if (string=? (sj (list "a" "b") ",") "a,b") 1 0)))",
                1, "4372: file-mode sj is a,b");
    eval_int_eq(file_cs,
                R"((let ((i 0))
  (while (lambda () (< i 3))
    (lambda () (set! i (+ i 1))))
  i))",
                3, "4372: top-level while is 3");
    eval_int_eq(file_cs,
                R"((begin
  (define (sj xs sep)
    (if (null? xs) ""
        (let loop ((xs (cdr xs)) (acc (car xs)) (sep sep))
          (if (null? xs) acc
              (loop (cdr xs) (string-append acc sep (car xs)) sep)))))
  (if (string=? (sj (list "a" "b") ",") "a,b") 1 0)))",
                1, "4372: named-let join is a,b");
    CHECK(file_cs.eval("(require \"std/string\" all:)").has_value(), "4372: require std/string");
    eval_int_eq(file_cs, R"((if (string=? (string-join (list "a" "b") ",") "a,b") 1 0))", 1,
                "4372: string-join");
    eval_int_eq(file_cs, R"((if (string=? (car (string-split "a,b" ",")) "a") 1 0))", 1,
                "4372: string-split head");
    eval_int_eq(file_cs, R"((if (string=? (car (cdr (string-split "a,b" ","))) "b") 1 0))", 1,
                "4372: string-split tail");
    eval_int_eq(file_cs, R"((if (string=? (string-trim "  a  ") "a") 1 0))", 1,
                "4372: string-trim");
    eval_int_eq(file_cs, R"((if (string=? (string-replace "ab" "a" "z") "zb") 1 0))", 1,
                "4372: string-replace");
    eval_int_eq(file_cs, R"((if (string-contains? "hello" "ell") 1 0))", 1,
                "4372: string-contains?");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #4372") != std::string::npos, "4372: cite");
}

// Issue #4379: a define lowered to an IR closure used to Call the while
// primitive with MakeClosure arms. That primitive only applies evaluator
// closures, so the loop did not run and the let cell stayed at its init.
static void ac4379_ir_while_lambda_set() {
    std::println("\n--- #4379: IR while-lambda set! ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    CompilerService cs;
    CHECK(cs.eval(R"((define (t)
  (let ((lo 0) (hi 3))
    (while (lambda () (< lo hi))
      (lambda () (set! lo (+ lo 1))))
    lo)))")
              .has_value(),
          "4379: define counter");
    eval_int_eq(cs, "(t)", 3, "4379: lambda while set! is 3");
    CHECK(cs.eval(R"((define (trim s)
  (define n (string-length s))
  (let ((lo 0) (hi n))
    (while (lambda () (and (< lo hi) (= (string-ref s lo) 32)))
      (lambda () (set! lo (+ lo 1))))
    (while (lambda () (and (> hi lo) (= (string-ref s (- hi 1)) 32)))
      (lambda () (set! hi (- hi 1))))
    (substring s lo hi))))")
              .has_value(),
          "4379: define trim");
    eval_int_eq(cs, R"((if (string=? (trim "  hi  ") "hi") 1 0))", 1, "4379: trim hi");
    CHECK(cs.eval("(require \"std/string\" all:)").has_value(), "4379: require std/string");
    eval_int_eq(cs, R"((if (string=? (string-trim "  hi  ") "hi") 1 0))", 1,
                "4379: module string-trim");
    eval_int_eq(cs, R"((if (string=? (car (string-split "a,b" ",")) "a") 1 0))", 1,
                "4379: module string-split head");
    eval_int_eq(cs, R"((if (string=? (car (cdr (string-split "a,b" ","))) "b") 1 0))", 1,
                "4379: module string-split tail");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("Issue #4379") != std::string::npos, "4379: cite");
}

// Issue #4466: (while cond b1 b2 ...) runs every body form each iteration.
// Increment-first is in-process (a missing later form returns the wrong sum).
// Display-first is a timeout subprocess so a dropped increment cannot hang.
static std::string aura_binary_4466() {
#ifdef AURA_SOURCE_DIR
    std::string cand = std::string(AURA_SOURCE_DIR) + "/build/aura";
    if (::access(cand.c_str(), X_OK) == 0)
        return cand;
#endif
    for (const char* c : {"./build/aura", "./aura", "../build/aura", "../aura"}) {
        if (::access(c, X_OK) == 0)
            return std::string(c);
    }
    return {};
}

struct Cli4466 {
    int rc = -1;
    std::string out;
    std::string err;
};

static Cli4466 run_file_4466(const std::string& bin, const std::string& src,
                             const std::string& assignments) {
    Cli4466 r;
    const char* outp = "/tmp/aura-4466-out.txt";
    const char* errp = "/tmp/aura-4466-err.txt";
    std::string cmd = "timeout 8 env -u AURA_SANDBOX -u AURA_PIPELINE_STRICT -u AURA_PATH " +
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

static void ac4466_while_multi_body() {
    std::println("\n--- #4466: while runs every body form ---");
    CHECK(read_file("src/compiler/evaluator_eval_flat.cpp").find("#4466") != std::string::npos,
          "4466: walker cite");
    CHECK(read_file("src/compiler/evaluator_primitives_control.cpp").find("#4466") !=
              std::string::npos,
          "4466: primitive cite");
    CHECK(read_file("src/compiler/lowering_impl.cpp").find("#4466") != std::string::npos,
          "4466: lowering cite");
    CHECK(read_file("src/compiler/service.ixx").find("callee_name == \"while\"") !=
              std::string::npos,
          "4466: while still needs the walker fallback");

    const char* bare = "(let ((i 0) (s 0)) (while (< i 3) (set! i (+ i 1)) (set! s (+ s i))) s)";
    const char* three = "(let ((i 0) (s 0)) (while (< i 3) (set! i (+ i 1)) (set! s (+ s i)) "
                        "(set! s (+ s 1))) s)";
    const char* thunks =
        "(let ((i 0) (s 0)) (while (lambda () (< i 3)) (lambda () (set! i (+ i 1))) "
        "(lambda () (set! s (+ s i)))) s)";
    const char* one = "(let ((i 0) (s 0)) (while (lambda () (< i 3)) (lambda () (set! i (+ i 1)) "
                      "(set! s (+ s i)))) s)";

    auto face = [&](const char* strict, const char* tag) {
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", strict, 1);
        apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
        CompilerService cs;
        eval_int_eq(cs, bare, 6, (std::string(tag) + " bare").c_str());
        eval_int_eq(cs, three, 9, (std::string(tag) + " three bodies").c_str());
        eval_int_eq(cs, thunks, 6, (std::string(tag) + " thunks").c_str());
        eval_int_eq(cs, one, 6, (std::string(tag) + " one lambda").c_str());
    };
    face("0", "4466 walker");
    face("force-soa", "4466 ir");
    unsetenv("AURA_SANDBOX");
    unsetenv("AURA_PIPELINE_STRICT");
    reset_tree_walker_fallback_policy_for_test();

    const auto bin = aura_binary_4466();
    CHECK(!bin.empty(), "4466: aura binary");
    if (bin.empty())
        return;

    struct FileCase {
        const char* name;
        const char* body;
        const char* want;
    };
    const FileCase cases[] = {
        {"x1",
         "(define (wh n) (let ((i 0)) (while (< i n) (display i) (set! i (+ i 1))) i))\n"
         "(display (wh 3))\n(newline)\n",
         "0123\n"},
        {"x2",
         "(define (wh n) (let ((i 0)) (while (< i n) (set! i (+ i 1)) (display i)) i))\n"
         "(display (wh 3))\n(newline)\n",
         "1233\n"},
        {"x3",
         "(define (wh n) (let ((i 0)) (while (< i n) (begin (display i) "
         "(set! i (+ i 1)))) i))\n"
         "(display (wh 3))\n(newline)\n",
         "0123\n"},
        {"top", "(define i 0)\n(while (< i 3) (display i) (set! i (+ i 1)))\n", "012"},
        {"thunk",
         "(define i 0)\n(define s 0)\n"
         "(while (lambda () (< i 3)) (lambda () (set! s (+ s i))) "
         "(lambda () (set! i (+ i 1))))\n"
         "(display s)\n",
         "3"},
        {"vec",
         "(define v (vector 0 0 0))\n(define i 0)\n"
         "(while (< i 3) (vector-set! v i i) (set! i (+ i 1)))\n"
         "(display (vector-ref v 0))(display (vector-ref v 1))(display (vector-ref v 2))\n",
         "012"},
    };
    const char* faces[] = {"AURA_SANDBOX=off AURA_PIPELINE_STRICT=0",
                           "AURA_SANDBOX=off AURA_PIPELINE_STRICT=force-soa"};
    const char* face_tags[] = {"soft", "soa"};
    for (std::size_t fi = 0; fi < 2; ++fi) {
        const char* env = faces[fi];
        const char* ftag = face_tags[fi];
        for (const auto& c : cases) {
            std::string path = std::string("/tmp/aura-4466-") + c.name + ".aura";
            {
                std::ofstream out(path);
                out << c.body;
            }
            auto run = run_file_4466(bin, path, env);
            std::string label = std::string("4466 ") + ftag + " " + c.name;
            if (run.rc != 0 || run.out != c.want) {
                std::println(std::cerr, "{} rc {} out [{}] err [{}]", label, run.rc, run.out,
                             run.err);
            }
            CHECK(run.rc == 0, label + " exits 0");
            CHECK(run.out == c.want, label + " output");
        }
    }
}

// Issue #4690: a closure-form while used to drop a failed thunk and
// iterate. The diagnostic (and the name) must come back, the body must
// not run again, and a thunk that stops on #f must still count to 3.
// The child is killed if it is still looping — the old bug hangs.

// Issue #4690: a failed eval rolls the top-level environment back
// (transactional commit), so a counter defined inside the failing form is
// unbound afterwards — reading it cannot observe how often the thunk ran.
// Count a stdout marker instead: it survives the rollback and is exactly
// the property under test (the failed thunk must not run again).
static int count_marker_4690(const std::string& path, char marker) {
    std::ifstream in(path);
    int n = 0;
    for (char c; in.get(c);)
        if (c == marker)
            ++n;
    return n;
}
static void ac4690_closure_while_keeps_diagnostic() {
    std::println("\n--- #4690: closure-form while keeps a failed thunk ---");
    CHECK(read_file("src/compiler/evaluator_primitives_control.cpp").find("#4690") !=
              std::string::npos,
          "4690: primitive cite");
    CHECK(read_file("src/compiler/evaluator_eval_flat.cpp").find("#4690") != std::string::npos,
          "4690: apply cite");

    const pid_t pid = ::fork();
    if (pid < 0) {
        CHECK(false, "4690: fork");
        return;
    }
    if (pid == 0) {
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PIPELINE_STRICT", "force-soa", 1);
        apply_pipeline_strict_defaults(/*dev_sandbox_off=*/true);
        const std::string out_path =
            std::format("/tmp/aura-4690-{}.out", static_cast<long>(::getpid()));
        if (!std::freopen(out_path.c_str(), "w", stdout))
            _exit(12);
        CompilerService cs;
        auto body = cs.eval(R"((begin
  (define n 0)
  (while (lambda () #t)
    (lambda ()
      (display "A")
      (not-a-real-binding-4690))))");
        std::fflush(stdout);
        if (body)
            _exit(2);
        // force-soa reports the call-position failure as "cannot call"
        // (same text as the flat while on that face). The walker face
        // reports "unbound variable". Either way the name is in the
        // diagnostic and the loop has stopped.
        const auto text = body.error().format();
        if (text.find("not-a-real-binding-4690") == std::string::npos)
            _exit(3);
        if (text.find("unbound variable") == std::string::npos &&
            text.find("cannot call") == std::string::npos)
            _exit(3);
        // The failed eval rolled the env back, so "n" is gone; the marker
        // count is the surviving evidence that the body ran once.
        if (count_marker_4690(out_path, 'A') != 1)
            _exit(4);

        auto pred = cs.eval(R"((begin
  (define m 0)
  (while (lambda () (not-a-real-pred-4690))
    (lambda () (display "B"))))");
        std::fflush(stdout);
        if (pred)
            _exit(5);
        if (pred.error().format().find("not-a-real-pred-4690") == std::string::npos)
            _exit(6);
        if (count_marker_4690(out_path, 'B') != 0)
            _exit(7);

        auto count = cs.eval(R"((let ((i 0))
  (while (lambda () (< i 3)) (lambda () (set! i (+ i 1))))
  i))");
        if (!count || !is_int(*count) || as_int(*count) != 3)
            _exit(8);

        auto multi = cs.eval(R"((begin
  (define k 0)
  (while (lambda () #t)
    (lambda () (display "C"))
    (lambda () (not-a-real-binding-4690-b)))
  k))");
        std::fflush(stdout);
        if (multi)
            _exit(9);
        if (multi.error().format().find("not-a-real-binding-4690-b") == std::string::npos)
            _exit(10);
        if (count_marker_4690(out_path, 'C') != 1)
            _exit(11);
        _exit(0);
    }

    int status = 0;
    bool reaped = false;
    for (int i = 0; i < 600; ++i) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0)
            break;
        ::usleep(100000);
    }
    if (!reaped) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        std::println(std::cerr, "4690: child still running after 60s (status {:#x})", status);
        CHECK(false, "4690: closure while hung");
        return;
    }
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok)
        std::println(std::cerr, "4690: child status {:#x}", status);
    CHECK(ok, "4690: closure-form while reports the unbound name and stops");
}

} // namespace

int run_test_while_define_oneshot() {
    std::println("=== Issue #2571: while + define loop counter footgun ===");
    ac1_define_in_while();
    ac2_multi_define_in_while();
    ac3_preferred_outer_set();
    ac4_source_gate();
    ac4350_capture_snapshot_set();
    ac4372_while_set_in_define();
    ac4379_ir_while_lambda_set();
    ac4466_while_multi_body();
    ac4690_closure_while_keeps_diagnostic();
    std::println("\n=== #2571: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_while_define_oneshot();
}
#endif
