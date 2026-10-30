// @category: unit
// @reason: Issue #4774 — aura::core links standalone (no Evaluator / runtime).
//
// This binary links ONLY the aura::core static library (contract handler +
// fiber_context) — no aura_test_objects, no evaluator, no reflect. It proves
// an embedder can use the header-only runtime pieces with one target:
//   AC1: PersistentChildVector COW (const with_set copies, rvalue in place)
//   AC2: OverlayRegistry begin / write / prepare / finalize (#4771 API)
//   AC3: fiber_ctx attach / detach round-trip and the plain fallback
//   AC4: a pre() violation reaches aura's handle_contract_violation (the
//        registered hook fires, then abort) — the handler comes from
//        aura::core, not from a stub the embedder had to copy

#include "compiler/fiber_cow_overlay.hh"
#include "core/fiber_context.hh"
#include "core/persistent_child_vector.hh"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

extern "C" {
struct AuraContractViolation {
    std::uint16_t kind;
    std::uint16_t semantic;
    std::uint16_t mode;
    const char* comment;
    const char* file;
    std::uint32_t line;
    const char* function;
};
using AuraContractViolationHook = void (*)(const AuraContractViolation*);
void aura_set_contract_violation_hook(AuraContractViolationHook fn);
}

namespace {
int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        std::printf("  PASS %s\n", what);
    } else {
        ++g_failed;
        std::printf("  FAIL %s\n", what);
    }
}

void ac1_pcv_cow() {
    std::printf("--- #4774 AC1: PersistentChildVector via aura::core\n");
    using V = aura::ast::PersistentChildVector<std::uint32_t>;
    V k;
    k = k.with_push_back(1).with_push_back(2).with_push_back(3);
    const V& ck = k;
    V k2 = ck.with_set(0, 9);
    check(ck[0] == 1 && k2[0] == 9, "AC1: const with_set copies, receiver unchanged");
    V r = std::move(k).with_set(1, 7);
    check(r.size() == 3 && r[1] == 7, "AC1: rvalue with_set publishes the write");
}

void ac2_overlay() {
    std::printf("--- #4774 AC2: OverlayRegistry via aura::core\n");
    aura::compiler::cow::OverlayRegistry<int> reg;
    std::unordered_map<std::string, int> shared;
    auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
    const void* owner = "embed";
    reg.begin(owner, 1, 0);
    reg.write(owner, 1, "x", 42, 0);
    const auto t = reg.prepare(owner, 1, 0);
    check(t.valid() && shared.empty(), "AC2: prepare reserves without applying");
    const auto f = reg.finalize(t, apply);
    check(f.ok && shared["x"] == 42 && reg.current_seq(owner) == 1,
          "AC2: finalize publishes and logs the claim");
}

void ac3_fiber_ctx() {
    std::printf("--- #4774 AC3: fiber_ctx via aura::core\n");
    namespace fc = aura::core::fiber_ctx;
    fc::Context* plain = &fc::active();
    check(plain == fc::plain_context(), "AC3: off-fiber active() is the plain fallback");
    fc::Context* c = fc::aura_fiber_ctx_create(7);
    check(c != nullptr, "AC3: create a fiber context");
    fc::aura_fiber_ctx_attach(c);
    check(&fc::active() == c, "AC3: attach installs the fiber context");
    fc::aura_fiber_ctx_detach();
    check(&fc::active() == plain, "AC3: detach restores the plain context");
    fc::aura_fiber_ctx_destroy(c);
}

int g_hook_fd = -1;
void hook(const AuraContractViolation* v) {
    const char msg = (v && v->kind == 1) ? 'P' : '?';
    (void)!::write(g_hook_fd, &msg, 1);
}

void ac4_contract_handler() {
    std::printf("--- #4774 AC4: pre() violation reaches aura's handler\n");
    int fds[2];
    if (::pipe(fds) != 0) {
        check(false, "AC4: pipe");
        return;
    }
    std::fflush(stdout); // the child must not re-emit buffered output
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        g_hook_fd = fds[1];
        aura_set_contract_violation_hook(&hook);
        aura::ast::PersistentChildVector<std::uint32_t> v;
        v = v.with_push_back(1);
        volatile std::size_t bad = 5;
        volatile std::uint32_t sink = v[bad]; // pre(i < size_) fails
        (void)sink;
        ::_exit(0); // reached only if the contract did not fire
    }
    ::close(fds[1]);
    char got = 0;
    const auto n = ::read(fds[0], &got, 1);
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    check(n == 1 && got == 'P', "AC4: the registered hook saw a pre() violation");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "AC4: handle_contract_violation aborts after the hook");
}
} // namespace

int main() {
    std::printf("=== #4774: aura::core standalone embed ===\n");
    ac1_pcv_cow();
    ac2_overlay();
    ac3_fiber_ctx();
    ac4_contract_handler();
    std::printf("\n=== #4774: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
