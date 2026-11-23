// Issue #5128: host the workspace bus in the owning Aura process.
//
// The builder lays out the delivered schema (image_format.hh /
// image_schema.hh): header, section directory, the five record channels
// and the constraints section. It mirrors the proven #5127 test builder
// with production capacities; the audit stays in the control-plane log
// (the image audit section is optional and skipped here, as in #5127).

#include "bus/host.hh"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

#include "bus/image_schema.hh"
#include "bus/transport.hh"

namespace aura::bus {
namespace {

    std::uint64_t fnv_bytes(std::span<const std::byte> bytes) {
        std::uint64_t h = 14695981039346656037ull;
        for (const auto b : bytes) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(b));
            h *= 1099511628211ull;
        }
        return h;
    }

    template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
        std::memcpy(img.data() + off, &v, sizeof(T));
    }

    bool build_host_image(std::span<std::byte> img, std::string* why) {
        constexpr std::uint32_t kSections = 8;
        constexpr std::uint64_t kMinPerChannel = 4096;
        constexpr std::uint64_t kConstraintBytes = 4096;
        // #5378: source-carrier string table — 1024 entry slots plus a
        // 64 KiB byte region for candidate define sources.
        constexpr std::uint64_t kStringSlots = 1024;
        constexpr std::uint64_t kStringBytes =
            ((kStringSlots * sizeof(StringTableEntry) + 7) & ~7ull) + (64u << 10);
        const std::uint64_t dir = kImageHeaderSize;
        const std::uint64_t elems[6] = {sizeof(MetricRecord), sizeof(CctRecord),
                                        sizeof(PatchRecord),  sizeof(WorldlineRecord),
                                        sizeof(NodeRecord),   sizeof(TaskRecord)};
        // #5387: channel capacity scales with the delivered image instead
        // of a fixed 4096 records. Each channel keeps at least 4096 slots;
        // space beyond that floor is split evenly. Exhaustion stays a hard
        // error — nothing grows after attach.
        std::uint64_t per[6];
        std::uint64_t floor_bytes = 0;
        for (int i = 0; i < 6; ++i) {
            per[i] = kMinPerChannel;
            floor_bytes += kMinPerChannel * elems[i];
        }
        const std::uint64_t fixed =
            dir + kSections * sizeof(SectionEntry) + kConstraintBytes + kStringBytes + 64;
        if (img.size() > fixed + floor_bytes) {
            const std::uint64_t extra = (img.size() - fixed - floor_bytes) / 6;
            for (int i = 0; i < 6; ++i) {
                const std::uint64_t add = std::min(
                    extra / elems[i], static_cast<std::uint64_t>(kImageMaxRecords) - per[i]);
                per[i] += add;
            }
        }
        std::uint64_t off = dir + kSections * sizeof(SectionEntry);
        auto take = [&](std::uint64_t bytes) {
            const std::uint64_t at = off;
            off = (off + bytes + 7) & ~7ull;
            return at;
        };
        const std::uint64_t metrics = take(per[0] * elems[0]);
        const std::uint64_t cct = take(per[1] * elems[1]);
        const std::uint64_t patches = take(per[2] * elems[2]);
        const std::uint64_t worldlines = take(per[3] * elems[3]);
        const std::uint64_t nodes = take(per[4] * elems[4]);
        const std::uint64_t constraints = take(kConstraintBytes);
        const std::uint64_t strings = take(kStringBytes);
        // #5399: the task board channel follows the existing sections.
        const std::uint64_t tasks = take(per[5] * elems[5]);
        const std::uint64_t committed = (off + 7) & ~7ull;
        if (committed > img.size()) {
            if (why)
                *why = "image too small for the host section layout";
            return false;
        }
        std::memset(img.data(), 0, img.size());

        ImageHeader h{};
        std::memcpy(h.magic, kImageMagic, sizeof(h.magic));
        h.abi_major = kImageAbiMajor;
        h.abi_minor = kImageAbiMinor;
        h.endianness = static_cast<std::uint32_t>(kImageEndianMarker);
        h.header_len = kImageHeaderSize;
        h.image_size = img.size();
        const SchemaId id = header_schema_id();
        std::memcpy(h.schema_id, &id.a, 8);
        std::memcpy(h.schema_id + 8, &id.b, 8);
        h.layout_version = kImageLayoutVersion;
        h.committed_bytes = committed;
        h.committed_gen = 2;
        h.section_dir_off = dir;
        h.section_count = kSections;
        put(img, 0, h);

        // The capacity is a parameter: the constraints section is not a
        // record channel and has no per[i] slot (reading one was an
        // out-of-bounds access that gave the section a garbage size).
        auto entry = [&](std::uint32_t i, SectionKind kind, std::uint64_t at, std::uint32_t elem,
                         std::uint64_t capacity) {
            SectionEntry e{};
            e.kind = static_cast<std::uint32_t>(kind);
            e.version = 1;
            e.offset = at;
            e.element_size = elem;
            e.capacity = capacity;
            e.generation = 2;
            e.fingerprint = section_layout_fingerprint(kind);
            e.digest = 14695981039346656037ull;
            put(img, dir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry), e);
        };
        entry(0, SectionKind::Metrics, metrics, sizeof(MetricRecord), per[0] * elems[0]);
        entry(1, SectionKind::Cct, cct, sizeof(CctRecord), per[1] * elems[1]);
        entry(2, SectionKind::Patches, patches, sizeof(PatchRecord), per[2] * elems[2]);
        entry(3, SectionKind::Worldlines, worldlines, sizeof(WorldlineRecord), per[3] * elems[3]);
        entry(4, SectionKind::Nodes, nodes, sizeof(NodeRecord), per[4] * elems[4]);
        entry(5, SectionKind::Constraints, constraints, sizeof(ConstraintSetHeader),
              kConstraintBytes);
        entry(7, SectionKind::Tasks, tasks, sizeof(TaskRecord), per[5] * elems[5]);
        {
            // String table: fixed entry array, then the byte region.
            SectionEntry se{};
            se.kind = static_cast<std::uint32_t>(SectionKind::StringTable);
            se.version = 1;
            se.offset = strings;
            se.element_size = sizeof(StringTableEntry);
            se.capacity = kStringBytes;
            se.length = (kStringSlots * sizeof(StringTableEntry) + 7) & ~7ull;
            se.count = 0;
            se.generation = 2;
            se.fingerprint = section_layout_fingerprint(SectionKind::StringTable);
            se.digest = fnv_bytes({img.data() + strings, static_cast<std::size_t>(se.length)});
            put(img, dir + 6 * sizeof(SectionEntry), se);
        }
        {
            SectionEntry ce{};
            std::memcpy(&ce, img.data() + dir + 5 * sizeof(SectionEntry), sizeof(ce));
            ce.capacity = kConstraintBytes;
            ConstraintSetHeader ch{};
            ch.version = 1;
            ch.object_off = sizeof(ConstraintSetHeader);
            ch.object_len = 0;
            ch.schema = 1;
            put(img, constraints, ch);
            ce.length = sizeof(ConstraintSetHeader);
            ce.count = 0;
            ce.digest = fnv_bytes({img.data() + constraints, sizeof(ConstraintSetHeader)});
            std::memcpy(img.data() + dir + 5 * sizeof(SectionEntry), &ce, sizeof(ce));
        }
        return true;
    }

    struct Host {
        Server server;
        std::thread worker;
        std::atomic<bool> stopping{false};
        std::string path;

        ~Host() {
            stopping.store(true);
            server.stop();
            if (worker.joinable())
                worker.join();
        }
    };

    std::mutex g_host_mutex;
    std::map<std::uint64_t, std::unique_ptr<Host>> g_hosts;

} // namespace

bool host_start(const HostOptions& opts, std::string* path_out, std::string* why) {
    const std::lock_guard lock(g_host_mutex);
    const auto it = g_hosts.find(opts.workspace);
    if (it != g_hosts.end()) {
        if (path_out)
            *path_out = it->second->path;
        return true;
    }
    auto host = std::make_unique<Host>();
    const std::string path = opts.path.empty() ? default_socket_path(opts.workspace) : opts.path;
    const std::size_t bytes = opts.image_bytes != 0 ? opts.image_bytes : kBusDefaultImageBytes;
    TransportError err;
    if (!host->server.listen_at(path, bytes, opts.workspace, &err)) {
        if (why)
            *why = err.message;
        return false;
    }
    // The workspace allow policy: the three downstream tools plus the
    // generic role, each capped at the full capability set; the sandbox
    // preset clamps at attach (ci drops constraint-write, prod grant).
    WorkspacePolicy policy;
    // #5400: planner/worker/supervisor are workspace roles too. Their
    // capabilities are the same preset subset; the role name never widens
    // the grant.
    for (const std::uint32_t role : {kRoleGeneric, kRolePerf, kRoleHarness, kRoleMaintainer,
                                     kRolePlanner, kRoleWorker, kRoleSupervisor})
        policy.allow(role, Capability{kCapAll, 0xFF, opts.workspace, 0, 0, 0});
    host->server.set_policy(policy, opts.preset);
    // Built-in rule names peers may install under constraint-write
    // (#5126): the delivered #5127 test contract and the image's
    // immediate invariant. The current set starts empty — constraints
    // are expert-installed, never baked in.
    host->server.register_constraint_predicate("allow-small",
                                               [](const ConstraintContext& ctx, std::string* w) {
                                                   if (ctx.new_value > 100) {
                                                       if (w)
                                                           *w = "new value exceeds 100";
                                                       return false;
                                                   }
                                                   return true;
                                               });
    host->server.register_constraint_predicate(
        "value-fits-immediate", [](const ConstraintContext& ctx, std::string* w) {
            if ((ctx.new_value >> 63) != 0) {
                if (w)
                    *w = "new value is not a legal immediate (bit 63 set)";
                return false;
            }
            return true;
        });
    // #5380: pre-registered safe predicate family for the pure-policy
    // closed loop. These are parameterized templates, not arbitrary peer
    // closures — the coordinator compiles them, peers only install by
    // name under constraint-write.
    //
    // pure-define-only: every source-carrier patch body must be a pure
    // computation — no I/O, network, FFI, mutation, global writes or bus
    // calls. Scans the candidate source text for banned operator names.
    host->server.register_constraint_predicate(
        "pure-define-only", [](const ConstraintContext& ctx, std::string* w) {
            static const std::string_view kBanned[] = {
                "display", "newline", "print",   "write",  "read-line", "read-char", "io-",
                "net-",    "socket-", "ffi",     "c-func", "load",      "eval",      "mutate:",
                "set!",    "bus:",    "require", "import", "open-",     "file-",     "delete-file",
                "system",  "exec",    "spawn",   "fork",
            };
            for (const auto& src : ctx.sources) {
                if (src.empty())
                    continue;
                for (const auto& bad : kBanned) {
                    if (src.find(bad) != std::string::npos) {
                        if (w)
                            *w = "impure operator in candidate body: " + std::string(bad);
                        return false;
                    }
                }
            }
            return true;
        });
    // target-allowlist: source-carrier patches may only target nodes in
    // the installed allowlist. #5401: the base predicate is fail-closed;
    // the install handler replaces it with the source-parameterized form.
    host->server.register_constraint_predicate(
        "target-allowlist", [](const ConstraintContext&, std::string* w) {
            if (w)
                *w = "target-allowlist is not installed with a node list";
            return false;
        });
    // metric-floor-bound: the trial's submitted metric (new_value) must
    // not fall below the installed floor. #5401: no floor means refuse.
    host->server.register_constraint_predicate(
        "metric-floor-bound", [](const ConstraintContext&, std::string* w) {
            if (w)
                *w = "metric-floor-bound is not installed with a floor";
            return false;
        });
    // v0.3.1 closed-loop gates (#5378/#5379/#5381): the host's live
    // workspace apply/revert callbacks, evidence-required KEEP and canary
    // observation. Defaults keep the v0.3.0 surface.
    if (opts.apply)
        host->server.set_apply_callback(opts.apply);
    if (opts.revert)
        host->server.set_revert_callback(opts.revert);
    // #5397: the host's define names, so a peer can address a named
    // function instead of guessing a host-local node id.
    if (opts.define_names)
        host->server.set_define_nodes_callback(opts.define_names);
    host->server.set_workspace_generation(opts.workspace_generation);
    host->server.set_evidence_required(opts.evidence_required);
    // #5391: the statistical admission policy (off by default).
    host->server.set_gate_policy(opts.gate);
    // #5393: the admission round budget and its checkpoint. The
    // checkpoint sits next to the socket, so a restarted host resumes
    // from it.
    host->server.set_round_budget(opts.round_budget);
    host->server.set_round_checkpoint_path(path + ".round");
    host->server.round_resume();
    // #5399: the task board resumes from the checkpoint next to the
    // socket. A malformed checkpoint fails the host closed rather than
    // silently losing claimed work.
    host->server.set_task_checkpoint_path(path + ".tasks");
    std::string task_why;
    if (!host->server.task_resume(&task_why)) {
        if (why)
            *why = task_why;
        return false;
    }
    // #5394: the degradation detector's configuration.
    host->server.set_degrade_config(opts.degrade);
    // #5396: the adversarial peer quotas.
    host->server.set_peer_quota(opts.peer_quota);
    // #5403: the task/global budget limits.
    host->server.set_budget_policy(opts.budget);
    host->server.set_canary_window(opts.canary_window);
    host->server.set_canary_floor(opts.canary_floor);
    host->server.set_canary_metric_key(opts.canary_metric_key);
    host->path = path;
    Host* raw = host.get();
    // #5377: one coordinator serves every attached peer concurrently.
    // Rejected attaches end only that connection (#5383); a peer crash
    // rolls back only its own pending trials and reclaims only its loans.
    raw->worker = std::thread([raw]() { raw->server.serve(build_host_image); });
    g_hosts[opts.workspace] = std::move(host);
    if (path_out)
        *path_out = path;
    return true;
}

bool host_stop(std::uint64_t workspace, std::string* why) {
    std::unique_ptr<Host> host;
    {
        const std::lock_guard lock(g_host_mutex);
        const auto it = g_hosts.find(workspace);
        if (it == g_hosts.end()) {
            if (why)
                *why = "workspace is not hosted";
            return false;
        }
        host = std::move(it->second);
        g_hosts.erase(it);
    }
    // ~Host stops the loop and joins; ~Server unlinks the socket.
    host.reset();
    return true;
}

bool host_active(std::uint64_t workspace) {
    const std::lock_guard lock(g_host_mutex);
    return g_hosts.find(workspace) != g_hosts.end();
}

} // namespace aura::bus
