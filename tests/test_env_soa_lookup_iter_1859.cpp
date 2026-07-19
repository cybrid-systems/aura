// @category: unit
// @reason: Issue #1859 — Env::lookup SoA parent_id_ walk must be
// iterative under one shared_lock (no recursive tmp.lookup that
// re-enters shared_mutex / grows C++ stack). Bounded by MAX_ENV_DEPTH.
//
//   AC1: source cites #1859; iterative while (no tmp.lookup recurse)
//   AC2: short SoA parent_id chain finds root binding
//   AC3: 600-hop SoA chain finds binding (no stack overflow / hang)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Env;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Build SoA chain: leaf → … → root (root binds "soa-var" = 99).
// Returns leaf EnvId (child of the chain start).
EnvId make_soa_chain(Evaluator& ev, std::size_t n_frames) {
    EnvId root = ev.alloc_env_frame(NULL_ENV_ID);
    ev.env_frame_mut(root).bind("soa-var", make_int(99));
    EnvId cur = root;
    for (std::size_t i = 1; i < n_frames; ++i)
        cur = ev.alloc_env_frame(cur);
    return cur; // leaf: parent_id chain of length n_frames-1 hops to root
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: iterative SoA walk (#1859) ---");
        std::string src;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1859") != std::string::npos, "cites #1859");
        auto pos = src.find("parent_id_ != NULL_ENV_ID");
        CHECK(pos != std::string::npos, "SoA parent_id_ branch present");
        // Include preceding #1859 comment (above the if) + walk body.
        auto win = src.substr(pos > 800 ? pos - 800 : 0, 3200);
        CHECK(win.find("while (cur != NULL_ENV_ID") != std::string::npos ||
                  win.find("while (cur != NULL_ENV_ID &&") != std::string::npos,
              "iterative while over parent_id chain");
        CHECK(win.find("hops") != std::string::npos, "hop counter");
        CHECK(win.find("MAX_ENV_DEPTH") != std::string::npos, "bounded by MAX_ENV_DEPTH");
        // Active code path (after the while) must not construct Env tmp.
        auto wpos = win.find("while (cur != NULL_ENV_ID");
        CHECK(wpos != std::string::npos, "while present for code-path scan");
        if (wpos != std::string::npos) {
            auto body = win.substr(wpos);
            // Strip line comments so pre-#1859 history in comments doesn't fail.
            std::string code;
            for (std::size_t i = 0; i < body.size();) {
                auto nl = body.find('\n', i);
                auto line = body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
                auto cmt = line.find("//");
                if (cmt != std::string::npos)
                    line = line.substr(0, cmt);
                code += line;
                code += '\n';
                if (nl == std::string::npos)
                    break;
                i = nl + 1;
            }
            CHECK(code.find("tmp.lookup") == std::string::npos, "no tmp.lookup in code");
            CHECK(code.find("Env tmp") == std::string::npos, "no Env tmp in code");
        }
        CHECK(win.find("shared_lock") != std::string::npos, "holds shared_lock once");
        CHECK(win.find("iteratively") != std::string::npos ||
                  win.find("not recursive") != std::string::npos,
              "documents iterative fix");
    }

    // ── AC2: short chain ──
    {
        std::println("\n--- AC2: short SoA parent_id chain ---");
        Evaluator ev;
        EnvId leaf_id = make_soa_chain(ev, 4); // root + 3 children
        Env leaf;
        leaf.set_owner(&ev);
        leaf.set_parent_id(leaf_id);
        // Local empty; resolve via SoA parent_id_ walk.
        auto v = leaf.lookup("soa-var");
        CHECK(v.has_value(), "found soa-var via short SoA chain");
        if (v)
            CHECK(is_int(*v) && as_int(*v) == 99, "value is 99");
        auto miss = leaf.lookup("no-such-soa-name");
        CHECK(!miss.has_value(), "missing name → nullopt");
    }

    // ── AC3: deep chain (would stack-overflow if recursive) ──
    {
        std::println("\n--- AC3: 600-hop SoA chain ---");
        Evaluator ev;
        EnvId leaf_id = make_soa_chain(ev, 600);
        Env leaf;
        leaf.set_owner(&ev);
        leaf.set_parent_id(leaf_id);
        auto v = leaf.lookup("soa-var");
        CHECK(v.has_value(), "found soa-var through 600 SoA hops");
        if (v)
            CHECK(is_int(*v) && as_int(*v) == 99, "deep value is 99");
        // Second walk still works (no stuck depth / lock).
        auto v2 = leaf.lookup("soa-var");
        CHECK(v2 && is_int(*v2) && as_int(*v2) == 99, "second deep SoA walk ok");
    }

    std::println("\n=== test_env_soa_lookup_iter_1859: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
