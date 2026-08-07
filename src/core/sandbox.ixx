// sandbox.ixx — module re-export of SandboxMode SSOT.
//
// Full implementation lives in sandbox.hh (single authority #2657).
// This unit only re-exports for C++ module consumers (resource_quota /
// lifetime_pin pattern). Prior dual definition of set_mode + separate
// g_sandbox_state object would have split-brain sandbox mode if any
// TU ever imported this module.
//
// Do NOT reintroduce a second set_mode / g_sandbox_mode_atomic body here.

module;
#include "core/sandbox.hh"

export module aura.core.sandbox;

import std;

export namespace aura::core::sandbox {

using ::aura::core::sandbox::g_sandbox_mode_atomic;
using ::aura::core::sandbox::g_sandbox_mode_authority_set_total;
using ::aura::core::sandbox::g_sandbox_state;
using ::aura::core::sandbox::is_sandbox_active;
using ::aura::core::sandbox::is_strict;
using ::aura::core::sandbox::kSandboxAuthorityIssue;
using ::aura::core::sandbox::kSandboxIssue;
using ::aura::core::sandbox::kSandboxPhase;
using ::aura::core::sandbox::SandboxAuthorityStats;
using ::aura::core::sandbox::SandboxMode;
using ::aura::core::sandbox::SandboxState;
using ::aura::core::sandbox::set_mode;
using ::aura::core::sandbox::snapshot_sandbox_authority_stats;

} // namespace aura::core::sandbox
