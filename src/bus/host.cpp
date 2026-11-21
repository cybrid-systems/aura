// Issue #5128: host the workspace bus in the owning Aura process.
//
// The builder lays out the delivered schema (image_format.hh /
// image_schema.hh): header, section directory, the five record channels
// and the constraints section. It mirrors the proven #5127 test builder
// with production capacities; the audit stays in the control-plane log
// (the image audit section is optional and skipped here, as in #5127).

#include "bus/host.hh"

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
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
        constexpr std::uint32_t kSections = 6;
        constexpr std::uint64_t kPerChannel = 4096;
        constexpr std::uint64_t kConstraintBytes = 4096;
        const std::uint64_t dir = kImageHeaderSize;
        std::uint64_t off = dir + kSections * sizeof(SectionEntry);
        auto take = [&](std::uint64_t bytes) {
            const std::uint64_t at = off;
            off = (off + bytes + 7) & ~7ull;
            return at;
        };
        const std::uint64_t metrics = take(kPerChannel * sizeof(MetricRecord));
        const std::uint64_t cct = take(kPerChannel * sizeof(CctRecord));
        const std::uint64_t patches = take(kPerChannel * sizeof(PatchRecord));
        const std::uint64_t worldlines = take(kPerChannel * sizeof(WorldlineRecord));
        const std::uint64_t nodes = take(kPerChannel * sizeof(NodeRecord));
        const std::uint64_t constraints = take(kConstraintBytes);
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
        h.layout_version = 1;
        h.committed_bytes = committed;
        h.committed_gen = 2;
        h.section_dir_off = dir;
        h.section_count = kSections;
        put(img, 0, h);

        auto entry = [&](std::uint32_t i, SectionKind kind, std::uint64_t at, std::uint32_t elem) {
            SectionEntry e{};
            e.kind = static_cast<std::uint32_t>(kind);
            e.version = 1;
            e.offset = at;
            e.element_size = elem;
            e.capacity = kPerChannel * elem;
            e.generation = 2;
            e.fingerprint = section_layout_fingerprint(kind);
            e.digest = 14695981039346656037ull;
            put(img, dir + static_cast<std::uint64_t>(i) * sizeof(SectionEntry), e);
        };
        entry(0, SectionKind::Metrics, metrics, sizeof(MetricRecord));
        entry(1, SectionKind::Cct, cct, sizeof(CctRecord));
        entry(2, SectionKind::Patches, patches, sizeof(PatchRecord));
        entry(3, SectionKind::Worldlines, worldlines, sizeof(WorldlineRecord));
        entry(4, SectionKind::Nodes, nodes, sizeof(NodeRecord));
        entry(5, SectionKind::Constraints, constraints, sizeof(ConstraintSetHeader));
        {
            // The nodes channel keeps a larger capacity than the record
            // channels: a workspace projects many more nodes than metrics.
            SectionEntry ne{};
            std::memcpy(&ne, img.data() + dir + 4 * sizeof(SectionEntry), sizeof(ne));
            ne.capacity = kPerChannel * sizeof(NodeRecord);
            std::memcpy(img.data() + dir + 4 * sizeof(SectionEntry), &ne, sizeof(ne));
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
    for (const std::uint32_t role : {kRoleGeneric, kRolePerf, kRoleHarness, kRoleMaintainer})
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
    host->path = path;
    Host* raw = host.get();
    raw->worker = std::thread([raw]() {
        TransportError serr;
        while (!raw->stopping.load()) {
            Attach peer;
            if (!raw->server.accept_one(build_host_image, &peer, &serr))
                break;
            // Serve this peer until it disconnects; the delivered loop
            // rolls back its pending trials and reclaims its loans.
            raw->server.wait_disconnect(&serr);
        }
    });
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
