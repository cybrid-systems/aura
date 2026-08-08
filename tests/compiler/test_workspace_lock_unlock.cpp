// @category: unit
// @reason: Issue #2786 — workspace:lock only updates ev.workspace_read_only_
// when the locked workspace is active (symmetric with workspace:unlock).
//
//   AC1: lock body guards workspace_read_only_ with active_idx (source)
//   AC2: unlock retains active_idx guard (source symmetry)
//   AC3: lock non-active does not block mutate on active root
//   AC4: lock active blocks mutate; unlock active restores mutate
//   AC5: unlock non-active leaves active lock / can-write? intact
//   AC6: this suite + linter; no docs/design/2786-*; no test_issue_2786.cpp

#include "test_harness.hpp"

#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
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

static std::string lock_window(const std::string& src) {
    auto pos = src.find("[\"workspace:lock\"]");
    if (pos == std::string::npos)
        pos = src.find("workspace:lock");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("[\"workspace:unlock\"]", pos);
    if (end == std::string::npos)
        end = src.find("workspace:unlock", pos);
    if (end == std::string::npos)
        end = pos + 900;
    return src.substr(pos, end - pos);
}

static std::string unlock_window(const std::string& src) {
    auto pos = src.find("[\"workspace:unlock\"]");
    if (pos == std::string::npos)
        pos = src.find("workspace:unlock");
    if (pos == std::string::npos)
        return {};
    auto end = src.find("workspace:can-write?", pos);
    if (end == std::string::npos)
        end = pos + 700;
    return src.substr(pos, end - pos);
}

// make_merr → pair (kind-string . (msg-string . ...)). Extract kind.
static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// P6 path: mutate:replace-pattern checks workspace_read_only_ before match.
// Success or non-read-only failure (e.g. not-found) both prove the quick
// flag did not deny the write. Only kind=="read-only" is the lock deny.
template <typename Expected>
static bool mutate_not_read_only_denied(CompilerService& cs, const Expected& r) {
    if (!r.has_value())
        return false;
    if (is_bool(*r))
        return true; // #t success or #f non-ro path
    if (is_pair(*r))
        return merr_kind(cs, *r) != "read-only";
    return true;
}

template <typename Expected>
static bool mutate_read_only_denied(CompilerService& cs, const Expected& r) {
    if (!r.has_value())
        return false;
    if (is_pair(*r))
        return merr_kind(cs, *r) == "read-only";
    return false;
}

static bool setup_ws(CompilerService& cs, const char* code) {
    if (!cs.eval(std::format("(set-code \"{}\")", code)).has_value())
        return false;
    if (!cs.eval("(eval-current)").has_value())
        return false;
    // workspace_tree_ is created on first :create (lock needs the tree).
    auto created = cs.eval("(workspace :create \"seed-2786\")");
    return created && is_int(*created) && as_int(*created) >= 1;
}

} // namespace

int run_test_workspace_lock_unlock() {
    std::println("=== Issue #2786: workspace:lock active-only quick flag ===");
    CHECK(true, "ac2786: issue stamp");

    // ── AC1/AC2: source shape ──
    {
        std::println("\n--- AC1/AC2: lock/unlock active_idx guards ---");
        auto src = read_file("src/compiler/evaluator_primitives_workspace.cpp");
        CHECK(!src.empty(), "AC1: workspace primitives readable");
        auto lwin = lock_window(src);
        auto uwin = unlock_window(src);
        CHECK(!lwin.empty(), "AC1: workspace:lock present");
        CHECK(!uwin.empty(), "AC2: workspace:unlock present");
        CHECK(lwin.find("Issue #2786") != std::string::npos, "AC1: cites #2786");
        CHECK(lwin.find("active_idx()") != std::string::npos, "AC1: lock checks active_idx");
        CHECK(lwin.find("workspace_read_only_") != std::string::npos,
              "AC1: lock updates workspace_read_only_");
        auto apos = lwin.find("active_idx()");
        auto rpos = lwin.find("workspace_read_only_");
        CHECK(apos != std::string::npos && rpos != std::string::npos && apos < rpos,
              "AC1: active_idx guard precedes quick-flag assign");
        CHECK(uwin.find("active_idx()") != std::string::npos, "AC2: unlock checks active_idx");
        CHECK(uwin.find("workspace_read_only_") != std::string::npos,
              "AC2: unlock updates workspace_read_only_");
    }

    // ── AC3: lock non-active must not poison active mutate path ──
    {
        std::println("\n--- AC3: lock non-active keeps active mutate writable ---");
        CompilerService cs;
        CHECK(setup_ws(cs, "(define x 1)"), "AC3: setup root+tree");
        auto created = cs.eval("(workspace :create \"lock-child-2786\")");
        CHECK(created && is_int(*created) && as_int(*created) >= 1, "AC3: create child");
        const auto child = as_int(*created);
        // Stay on root (active=0). Lock non-active child read-only.
        auto lk = cs.eval(std::format("(workspace :lock {} #t)", child));
        CHECK(lk && is_bool(*lk) && as_bool(*lk), "AC3: lock non-active #t");
        // Active root must still accept mutate (quick flag not flipped).
        // replace-pattern checks workspace_read_only_ (P6 path) before match.
        auto mut = cs.eval("(mutate:replace-pattern \"(define x 1)\" \"(define x 2)\")");
        CHECK(mutate_not_read_only_denied(cs, mut),
              "AC3: mutate on active root not read-only-denied after locking non-active");
        auto cw = cs.eval("(workspace:can-write?)");
        CHECK(cw && is_bool(*cw) && as_bool(*cw), "AC3: can-write? active still #t");
        auto cw_child = cs.eval(std::format("(workspace:can-write? {})", child));
        CHECK(cw_child && is_bool(*cw_child) && !as_bool(*cw_child),
              "AC3: can-write? child #f after lock");
    }

    // ── AC4: lock active blocks mutate; unlock restores ──
    {
        std::println("\n--- AC4: lock active blocks mutate; unlock restores ---");
        CompilerService cs;
        CHECK(setup_ws(cs, "(define y 10)"), "AC4: setup");
        auto cur = cs.eval("(workspace :current)");
        CHECK(cur && is_int(*cur) && as_int(*cur) == 0, "AC4: active is 0");
        auto lk = cs.eval("(workspace :lock 0 #t)");
        CHECK(lk && is_bool(*lk) && as_bool(*lk), "AC4: lock active #t");
        auto cw = cs.eval("(workspace:can-write?)");
        CHECK(cw && is_bool(*cw) && !as_bool(*cw), "AC4: can-write? #f when active locked");
        auto mut_denied = cs.eval("(mutate:replace-pattern \"(define y 10)\" \"(define y 11)\")");
        CHECK(mutate_read_only_denied(cs, mut_denied),
              "AC4: mutate read-only-denied on active lock");
        auto un = cs.eval("(workspace :unlock 0)");
        CHECK(un && is_bool(*un) && as_bool(*un), "AC4: unlock active #t");
        auto cw2 = cs.eval("(workspace:can-write?)");
        CHECK(cw2 && is_bool(*cw2) && as_bool(*cw2), "AC4: can-write? restored #t");
        auto mut_ok = cs.eval("(mutate:replace-pattern \"(define y 10)\" \"(define y 12)\")");
        CHECK(mutate_not_read_only_denied(cs, mut_ok),
              "AC4: mutate not read-only-denied after unlock");
    }

    // ── AC5: unlock non-active does not clear active lock ──
    {
        std::println("\n--- AC5: unlock non-active leaves active lock intact ---");
        CompilerService cs;
        CHECK(setup_ws(cs, "(define z 3)"), "AC5: setup");
        auto created = cs.eval("(workspace :create \"unlock-child-2786\")");
        CHECK(created && is_int(*created), "AC5: create");
        const auto child = as_int(*created);
        auto lk = cs.eval("(workspace :lock 0 #t)");
        CHECK(lk && is_bool(*lk) && as_bool(*lk), "AC5: lock active");
        // Unlock non-active child — must not clear active quick flag.
        auto un = cs.eval(std::format("(workspace :unlock {})", child));
        CHECK(un && is_bool(*un) && as_bool(*un), "AC5: unlock non-active");
        auto cw = cs.eval("(workspace:can-write?)");
        CHECK(cw && is_bool(*cw) && !as_bool(*cw),
              "AC5: active still read-only after unlocking non-active");
        auto mut = cs.eval("(mutate:replace-pattern \"(define z 3)\" \"(define z 4)\")");
        CHECK(mutate_read_only_denied(cs, mut),
              "AC5: mutate still read-only-denied after unlock non-active");
        CHECK(cs.eval("(workspace :unlock 0)").has_value(), "AC5: unlock active cleanup");
    }

    std::println("\n=== #2786 workspace lock/unlock: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_workspace_lock_unlock();
}
#endif
