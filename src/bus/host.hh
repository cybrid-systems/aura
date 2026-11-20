#ifndef AURA_BUS_HOST_HH
#define AURA_BUS_HOST_HH

// Issue #5128: the workspace owner hosts the bus. #5121's process model
// makes the Aura process that owns the live workspace the coordinator —
// no separate daemon. std/bus was client-only (#5127); this wires the
// delivered Server into a background host the owning process can start
// from Aura: (bus:host workspace [image-bytes] [path]).
//
// One peer is served at a time (accept -> serve -> re-accept). Simultaneous
// multi-peer sessions are the follow-up; the serving loop, capability
// checks, loans, constraints and audit are the delivered ones, unchanged.

#include <cstddef>
#include <cstdint>
#include <string>

#include "bus/capability.hh"

namespace aura::bus {

// Tool role ids for the workspace allow policy (#5128 contract). Roles
// are hints at attach; the policy decides, and unknown roles are denied.
inline constexpr std::uint32_t kRoleGeneric = 0;
inline constexpr std::uint32_t kRolePerf = 1;
inline constexpr std::uint32_t kRoleHarness = 2;
inline constexpr std::uint32_t kRoleMaintainer = 3;

struct HostOptions {
    std::uint64_t workspace = 0;
    std::size_t image_bytes = 0; // 0 = kBusDefaultImageBytes
    std::string path;            // empty = default_socket_path(workspace)
    SandboxPreset preset = SandboxPreset::Dev;
};

// Start hosting the workspace bus in this process. Idempotent per
// workspace: a second call returns the existing path. The host serves
// peers until host_stop or process exit; a disconnect rolls back the
// peer's pending trials and reclaims its loans (the delivered semantics).
bool host_start(const HostOptions& opts, std::string* path_out, std::string* why);

// Stop hosting: unblock the accept/serve loop, join it, unlink the socket.
bool host_stop(std::uint64_t workspace, std::string* why);

bool host_active(std::uint64_t workspace);

} // namespace aura::bus

#endif
