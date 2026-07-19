// @category: unit
// @reason: Issue #1754 — is_env_frame_stale must not conflate NULL/OOB
// with version-staleness; is_env_frame_invalid_id covers missing frames.
//
//   AC1: source cites #1754; is_env_frame_invalid_id present
//   AC2: NULL / OOB → invalid_id true, stale false
//   AC3: live fresh frame → neither invalid_id nor stale
//   AC4: live frame after defuse bump → stale true, invalid_id false

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: source cites #1754 + invalid_id ---");
        std::string env;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            env = read_file(p);
            if (!env.empty())
                break;
        }
        CHECK(!env.empty(), "read evaluator_env.cpp");
        CHECK(env.find("#1754") != std::string::npos, "cites #1754");
        CHECK(env.find("is_env_frame_invalid_id") != std::string::npos,
              "uses is_env_frame_invalid_id");

        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty() && ixx.find("is_env_frame_invalid_id") != std::string::npos,
              "invalid_id declared in evaluator.ixx");
    }

    // ── AC2: NULL / OOB ──
    {
        std::println("\n--- AC2: NULL/OOB are invalid_id, not stale ---");
        Evaluator ev;
        CHECK(ev.is_env_frame_invalid_id(NULL_ENV_ID), "NULL invalid_id");
        CHECK(!ev.is_env_frame_stale(NULL_ENV_ID), "NULL not stale");
        CHECK(ev.is_env_frame_invalid_id(999999), "OOB invalid_id");
        CHECK(!ev.is_env_frame_stale(999999), "OOB not stale");
        CHECK(!ev.is_valid_env_id(NULL_ENV_ID), "NULL not valid_env_id");
        CHECK(!ev.is_valid_env_id(999999), "OOB not valid_env_id");
    }

    // ── AC3: fresh live frame ──
    {
        std::println("\n--- AC3: fresh frame neither invalid_id nor stale ---");
        Evaluator ev;
        EnvId id = ev.alloc_env_frame();
        CHECK(id != NULL_ENV_ID, "alloc");
        CHECK(!ev.is_env_frame_invalid_id(id), "live not invalid_id");
        CHECK(!ev.is_env_frame_stale(id), "fresh not stale");
        CHECK(ev.is_valid_env_id(id), "live is valid_env_id");
    }

    // ── AC4: version-stale live frame ──
    {
        std::println("\n--- AC4: defuse bump → stale, still valid id ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        EnvId id = ev.alloc_env_frame();
        CHECK(!ev.is_env_frame_stale(id), "fresh");
        // Bump defuse so stamped frame.version_ is behind.
        for (int i = 0; i < 3; ++i)
            ev.bump_defuse_version_for_test();
        CHECK(!ev.is_env_frame_invalid_id(id), "still a valid slot");
        CHECK(ev.is_env_frame_stale(id), "version-stale after defuse bump");
    }

    std::println("\n=== test_env_frame_stale_vs_invalid_id_1754: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
