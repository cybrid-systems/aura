#ifndef AURA_BUS_HOST_HH
#define AURA_BUS_HOST_HH

// Issue #5128: the workspace owner hosts the bus. #5121's process model
// makes the Aura process that owns the live workspace the coordinator —
// no separate daemon. std/bus was client-only (#5127); this wires the
// delivered Server into a background host the owning process can start
// from Aura: (bus:host workspace [image-bytes] [path]).
//
// #5377: peers are served concurrently from one coordinator poll loop;
// a session's crash/EOF reclaims only its own loans and pending trials.
// The serving loop, capability checks, loans, constraints and audit are
// the delivered ones, unchanged.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>

#include "bus/capability.hh"
#include "bus/degrade.hh"
#include "bus/gate.hh"
#include "bus/image_format.hh"
#include "bus/round.hh"
#include "bus/transport.hh"

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
    // #5378/#5379/#5381 (v0.3.1 closed loop): evidence-gated KEEP and
    // canary observation. Defaults keep the v0.3.0 surface.
    bool evidence_required = false;
    std::uint64_t canary_window = 0;     // metric ticks; 0 = no observation
    std::uint64_t canary_floor = 0;      // metric floor; 0 = no auto-revert
    std::uint64_t canary_metric_key = 0; // which metric key drives the window
    // #5391: the statistical admission policy. Defaults keep the
    // v0.3.1 surface: the gate is off until a host asks for it.
    GatePolicy gate{};
    // #5393: the admission round budget (wall clock, cooldown, the
    // round cap). The checkpoint lives next to the socket.
    RoundBudget round_budget{};
    // #5394: the degradation detector's configuration. Without watched
    // keys the detector stays quiet.
    DegradeConfig degrade{};
    // #5396: the adversarial peer quotas. The defaults are generous —
    // a well-behaved peer never reaches them — but a flood, a stalled
    // handshake or a constraint-write loop is cut off.
    PeerQuota peer_quota{};
    // #5378: apply a KEEP'd trial's patches to the live workspace. Runs
    // on the coordinator thread; must be all-or-nothing (byte-identical
    // on failure). canary=true means the apply enters observation and a
    // revert callback may later restore the pre-apply snapshot.
    std::function<bool(std::uint64_t worldline, std::uint64_t generation,
                       std::span<const PatchRecord> patches, std::span<const std::string> sources,
                       bool canary, std::string* why)>
        apply;
    // #5381: restore the pre-canary snapshot when an observation reverts.
    std::function<bool(std::uint64_t worldline, std::string* why)> revert;
    // #5397: the host's live define names and their node ids. A peer
    // that wants to rewrite a named function (choose-fn + admit-fn +
    // ttl-fn) asks the host for the id; the ids are host-local, so
    // guessing them from another process is not sound.
    std::function<bool(std::vector<std::pair<std::string, std::uint64_t>>* out)> define_names;
    // The live workspace generation at host time (trials bind to it).
    std::uint64_t workspace_generation = 0;
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
