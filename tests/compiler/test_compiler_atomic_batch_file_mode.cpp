// @category: unit
// @reason: Issue #4476 — mutate:atomic-batch inside a top-level form had two
//          faces in Soft file mode:
//
//   A. A two-op batch + ast:restore + eval-current inside ONE top-level form
//      made the file runner re-run the whole file from the first form,
//      silently, rc=0 (every earlier side effect doubled).
//   B. (define x (mutate:atomic-batch ...)) applied the batch but left x
//      unbound — the lockless batch rebind's env refresh re-keyed top_env()
//      onto the workspace pool and never restored it, so the caller-frame
//      bind_symid mirrored through the wrong pool and the next re-key
//      dropped the binding.
//
//   AC1: repro A file prints start once, then (end 7 2 0), exit 0
//   AC2: repro B file prints (1 3 3) then #t, exit 0
//   AC3: source-cite on the fix (evaluator_eval_flat.cpp)
//
// CLI file mode, matching the filed repro (aura <file.aura>).

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/wait.h>

import std;

using aura::test::g_failed;
using aura::test::g_passed;

namespace {

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

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
    for (const char* c : {"./aura", "./build/aura", "../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

// Runs the CLI on a file for one face. Returns exit code; stdout via out.
int run_cli(const std::string& bin, const char* src_path, const char* out_path,
            const std::string& face_env, std::string& out) {
    std::string cmd = "timeout 20 " + face_env + " '" + bin + "' '" + src_path + "' > '" +
                      out_path + "' 2>/dev/null";
    const int raw = std::system(cmd.c_str());
    std::ifstream in(out_path);
    out.assign((std::istreambuf_iterator<char>(in)), {});
    return (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
}

// Issue #4476 AC1: a top-level form that runs a two-op mutate:atomic-batch
// and then ast:restore + eval-current runs the file exactly once.
void ac4476_cli_batch_restore_once() {
    std::printf("\n--- #4476 AC1: two-op batch + restore runs the file once ---\n");
    const auto bin = aura_binary();
    CHECK(!bin.empty(), "4476: aura binary");
    if (bin.empty())
        return;
    const char* src_path = "/tmp/aura-4476-repro-a.aura";
    const char* out_path = "/tmp/aura-4476-a-out.txt";
    {
        std::ofstream f(src_path);
        f << "(display \"start\") (newline)\n";
        f << "(set-code \"(define k 0)\\n(define f (lambda (n) (+ n 1)))\")\n";
        f << "(eval-current)\n";
        f << "(define s0 (ast:snapshot \"p\"))\n";
        f << "(define (go)\n";
        f << "  (mutate:atomic-batch (list (list \"mutate:rebind\" \"k\" "
             "\"(lambda (n) (+ n 10))\" \"a\")\n";
        f << "                             (list \"mutate:rebind\" \"f\" "
             "\"(lambda (a0) (k a0))\" \"b\")))\n";
        f << "  (ast:restore s0)\n";
        f << "  (eval-current)\n";
        f << "  7)\n";
        f << "(define r #f)\n";
        f << "(set! r (go))\n";
        f << "(display (list \"end\" r (f 1) k)) (newline)\n";
    }
    // Filed repro face: AURA_SANDBOX=off AURA_PIPELINE_STRICT=0. The default
    // sandbox face denies kEffectMutate to scripts (capability contract), so
    // the self-modifying-file face is the sandbox-off one.
    const char* faces[][2] = {
        {"env AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "repro-face"},
    };
    for (const auto& face : faces) {
        std::string out;
        const int rc = run_cli(bin, src_path, out_path, face[0], out);
        CHECK(rc == 0, "4476 AC1: exit 0");
        if (out != "start\n(end 7 2 0)\n") {
            std::printf("  FAIL AC1 face=%s rc=%d out=[%s]\n", face[1], rc, out.c_str());
            ++g_failed;
        } else {
            ++g_passed;
            std::printf("  PASS: 4476 AC1 %s runs once\n", face[1]);
        }
    }
}

// Issue #4476 AC2: a define whose RHS runs a mutate:atomic-batch is bound.
void ac4476_cli_define_rhs_batch_bound() {
    std::printf("\n--- #4476 AC2: define with batch RHS binds ---\n");
    const auto bin = aura_binary();
    CHECK(!bin.empty(), "4476: aura binary");
    if (bin.empty())
        return;
    const char* src_path = "/tmp/aura-4476-repro-b.aura";
    const char* out_path = "/tmp/aura-4476-b-out.txt";
    {
        std::ofstream f(src_path);
        f << "(set-code \"(define (f x) (+ x 1))\")\n";
        f << "(eval-current)\n";
        f << "(define before 1)\n";
        f << "(define r2 (mutate:atomic-batch (list (list \"mutate:rebind\" \"f\" "
             "\"(lambda (x) (+ x 2))\" \"b\"))))\n";
        f << "(define after 3)\n";
        f << "(display (list before after (f 1))) (newline)\n";
        f << "(display r2) (newline)\n";
    }
    // Same filed repro face as AC1 (see note there).
    const char* faces[][2] = {
        {"env AURA_SANDBOX=off AURA_PIPELINE_STRICT=0", "repro-face"},
    };
    for (const auto& face : faces) {
        std::string out;
        const int rc = run_cli(bin, src_path, out_path, face[0], out);
        CHECK(rc == 0, "4476 AC2: exit 0");
        if (out != "(1 3 3)\n#t\n") {
            std::printf("  FAIL AC2 face=%s rc=%d out=[%s]\n", face[1], rc, out.c_str());
            ++g_failed;
        } else {
            ++g_passed;
            std::printf("  PASS: 4476 AC2 %s r2 bound\n", face[1]);
        }
    }
}

// Issue #4476 AC3: the fix is source-cited on the touched surface.
void ac4476_source_cite() {
    std::printf("\n--- #4476 AC3: source cite ---\n");
    const auto eval_flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(eval_flat.find("Issue #4476") != std::string::npos,
          "4476: cite on the lockless rebind env refresh");
}

} // namespace

int main() {
    std::printf("=== Issue #4476: atomic-batch top-level file mode ===\n");
    ac4476_cli_batch_restore_once();
    ac4476_cli_define_rhs_batch_bound();
    ac4476_source_cite();
    std::println("Total: {} passed, {} failed", ::aura::test::g_passed, ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}
