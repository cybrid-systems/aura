// @category: unit
// @reason: Issue #1756 — resolve_env_frame_detailed distinguishes
// NULL_ID / OOB / INVALID_VERSION / OK (not a single nullptr).
//
//   AC1: source cites #1756; EnvFrameResolveStatus + detailed APIs
//   AC2: NULL_ENV_ID → NULL_ID, frame == nullptr
//   AC3: OOB → OOB, frame == nullptr
//   AC4: live fresh frame → OK + non-null
//   AC5: INVALID_VERSION slot → INVALID_VERSION, detailed null frame;
//        thin resolve_env_frame still returns pointer (BC)
//   AC6: thin resolve_env_frame null only for NULL/OOB

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::EnvFrameResolveStatus;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::INVALID_VERSION;
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
        std::println("\n--- AC1: #1756 detailed resolve API ---");
        std::string env;
        for (const char* p :
             {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
            env = read_file(p);
            if (!env.empty())
                break;
        }
        CHECK(!env.empty(), "read evaluator_env.cpp");
        CHECK(env.find("#1756") != std::string::npos, "cites #1756");
        CHECK(env.find("resolve_env_frame_detailed") != std::string::npos, "detailed impl");
        CHECK(env.find("EnvFrameResolveStatus::NULL_ID") != std::string::npos, "NULL_ID status");
        CHECK(env.find("EnvFrameResolveStatus::OOB") != std::string::npos, "OOB status");
        CHECK(env.find("EnvFrameResolveStatus::INVALID_VERSION") != std::string::npos,
              "INVALID_VERSION status");

        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty() && ixx.find("enum class EnvFrameResolveStatus") != std::string::npos,
              "status enum exported");
    }

    // ── AC2/AC3/AC6: NULL + OOB ──
    {
        std::println("\n--- AC2/AC3/AC6: NULL and OOB ---");
        Evaluator ev;
        auto n = ev.resolve_env_frame_detailed(NULL_ENV_ID);
        CHECK(n.status == EnvFrameResolveStatus::NULL_ID, "NULL → NULL_ID");
        CHECK(n.frame == nullptr, "NULL frame null");
        CHECK(!n, "NULL not ok");
        CHECK(ev.resolve_env_frame(NULL_ENV_ID) == nullptr, "thin NULL null");

        auto o = ev.resolve_env_frame_detailed(999999);
        CHECK(o.status == EnvFrameResolveStatus::OOB, "OOB → OOB");
        CHECK(o.frame == nullptr, "OOB frame null");
        CHECK(ev.resolve_env_frame(999999) == nullptr, "thin OOB null");
    }

    // ── AC4: live OK ──
    {
        std::println("\n--- AC4: live fresh frame OK ---");
        Evaluator ev;
        EnvId id = ev.alloc_env_frame();
        auto r = ev.resolve_env_frame_detailed(id);
        CHECK(r.status == EnvFrameResolveStatus::OK, "status OK");
        CHECK(r.frame != nullptr, "frame non-null");
        CHECK(static_cast<bool>(r), "bool true");
        CHECK(ev.resolve_env_frame(id) == r.frame, "thin matches detailed");
    }

    // ── AC5: INVALID_VERSION ──
    {
        std::println("\n--- AC5: INVALID_VERSION ---");
        Evaluator ev;
        EnvId id = ev.alloc_env_frame();
        // Soft-mark via invalidate_post_rollback path: set checkpoint
        // below size then invalidate post-rollback frames.
        const auto base = ev.env_frames_size();
        (void)ev.alloc_env_frame(); // grow
        ev.set_panic_safe_env_frames_size_for_test(base);
        // Mark frames past checkpoint INVALID without shrink first.
        ev.invalidate_post_rollback_env_frames();
        // The doomed frame (last alloc) should be INVALID_VERSION.
        EnvId doomed = static_cast<EnvId>(ev.env_frames_size() - 1);
        CHECK(ev.is_env_frame_invalid(doomed), "doomed invalid");
        auto d = ev.resolve_env_frame_detailed(doomed);
        CHECK(d.status == EnvFrameResolveStatus::INVALID_VERSION, "detailed INVALID_VERSION");
        CHECK(d.frame == nullptr, "detailed nulls poison frame");
        // Thin resolve keeps BC: still returns a pointer for in-range slot.
        CHECK(ev.resolve_env_frame(doomed) != nullptr, "thin still returns in-range pointer");
    }

    std::println("\n=== test_resolve_env_frame_detailed_1756: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
