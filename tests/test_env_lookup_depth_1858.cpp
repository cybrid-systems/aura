// @category: unit
// @reason: Issue #1858 — Env::lookup depth counter is one increment
// per frame (not 2× on parent recursion). A parent chain of length
// N peaks at depth N; MAX_ENV_DEPTH=1024 is not halved to 512.
//
//   AC1: source cites #1858; documents one-per-frame semantics
//   AC2: chain of 600 parents: root binding still found (would fail
//        if effective limit were 512 via double-count)
//   AC3: after deep lookup, counter returns to 0 (RAII balance)

#include "test_harness.hpp"

#include <fstream>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Env;
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

// Build a chain leaf -> ... -> root where root binds "deep-var".
// Returns owning storage for all Envs (leaf is index 0).
struct Chain {
    std::vector<std::unique_ptr<Env>> envs; // [0]=leaf, back()=root
    Env* leaf() { return envs.front().get(); }
    Env* root() { return envs.back().get(); }
};

Chain make_parent_chain(std::size_t n_frames) {
    Chain c;
    c.envs.reserve(n_frames);
    // Root first in construction: push root, then children pointing up.
    // We'll store leaf at [0]: build from root toward leaf then reverse,
    // or link as we go from leaf toward root.
    // Easiest: allocate all, then wire parent_ of i to i+1, last is root.
    for (std::size_t i = 0; i < n_frames; ++i)
        c.envs.push_back(std::make_unique<Env>());
    for (std::size_t i = 0; i + 1 < n_frames; ++i)
        c.envs[i]->set_parent(c.envs[i + 1].get());
    // Bind on root only.
    c.root()->bind("deep-var", make_int(42));
    return c;
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: #1858 one-per-frame depth semantics ---");
        std::string src;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator_env.cpp");
        CHECK(src.find("#1858") != std::string::npos, "cites #1858");
        CHECK(src.find("MAX_ENV_DEPTH") != std::string::npos, "has MAX_ENV_DEPTH");
        CHECK(src.find("1024") != std::string::npos, "limit is 1024 (not blindly doubled)");
        CHECK(src.find("not 2N") != std::string::npos ||
                  src.find("peaks at N") != std::string::npos ||
                  src.find("not halved") != std::string::npos,
              "documents correct N-not-2N semantics");
        CHECK(src.find("g_env_lookup_depth") != std::string::npos, "depth counter present");
        CHECK(src.find("parent_->lookup") != std::string::npos, "recursive parent walk");
    }

    // ── AC2: chain of 600 > 512 ──
    // If double-counting were real, peak depth for 600 frames would be
    // 1200 > 1024 and lookup would fail. Under correct semantics peak=600.
    {
        std::println("\n--- AC2: 600-frame parent chain still finds binding ---");
        constexpr std::size_t kChain = 600;
        auto chain = make_parent_chain(kChain);
        auto v = chain.leaf()->lookup("deep-var");
        CHECK(v.has_value(), "lookup found deep-var through 600 parents");
        if (v) {
            CHECK(is_int(*v) && as_int(*v) == 42, "value is 42");
        }
        // Missing name walks full chain then nullopt — must not hang.
        auto miss = chain.leaf()->lookup("no-such-binding-xyz");
        CHECK(!miss.has_value(), "missing name → nullopt after full walk");
    }

    // ── AC3: counter balanced after deep walk ──
    {
        std::println("\n--- AC3: second deep lookup still works (depth reset) ---");
        auto chain = make_parent_chain(400);
        auto a = chain.leaf()->lookup("deep-var");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "first walk ok");
        auto b = chain.leaf()->lookup("deep-var");
        CHECK(b && is_int(*b) && as_int(*b) == 42, "second walk ok (RAII restored depth)");
        // Short chain still works.
        auto short_c = make_parent_chain(3);
        auto c = short_c.leaf()->lookup("deep-var");
        CHECK(c && is_int(*c) && as_int(*c) == 42, "short chain ok");
    }

    std::println("\n=== test_env_lookup_depth_1858: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
