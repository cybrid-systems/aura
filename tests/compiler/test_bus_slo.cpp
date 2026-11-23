// Issue #5395: the bus's own performance counters and the capacity
// plan.
//
// The counters measure the bus's overhead in CPU time — never the wall
// clock, which on a shared runner measures the scheduler and not the
// work (#5324). The capacity plan answers how much room is left in the
// image and how many rounds that room still supports.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/image_schema.hh"
#include "bus/slo.hh"
#include "bus/transport.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

// A small image: the channels hold a handful of records, so the
// capacity plan has something to say.
bool build_small_image(std::span<std::byte> img, std::string*) {
    if (img.size() < 64 * 1024)
        return false;
    std::memset(img.data(), 0, img.size());
    constexpr std::uint64_t kRecords = 4;
    const std::uint64_t dir = 256;
    const std::uint64_t entries = 2 * sizeof(SectionEntry);
    const std::uint64_t metrics_at = dir + entries;
    const std::uint64_t worldlines_at = metrics_at + kRecords * sizeof(MetricRecord);
    // The committed region covers the section's whole capacity.
    const std::uint64_t committed = worldlines_at + 2 * kRecords * sizeof(WorldlineRecord);

    ImageHeader h{};
    std::memcpy(h.magic, kImageMagic, sizeof(kImageMagic));
    h.abi_major = kImageAbiMajor;
    h.abi_minor = kBusAbiMinor;
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
    h.section_count = 2;
    std::memcpy(img.data(), &h, sizeof(h));

    // The worldline records: an empty lineage, so the parent is the
    // "no parent" marker.
    for (std::uint64_t i = 0; i < kRecords; ++i) {
        WorldlineRecord w{};
        w.id = i + 1;
        w.parent = kImageNoParent;
        w.state = static_cast<std::uint8_t>(WorldlineState::Pending);
        w.metrics_off = kImageNoIndex;
        std::memcpy(img.data() + worldlines_at + i * sizeof(WorldlineRecord), &w, sizeof(w));
    }

    SectionEntry metrics{};
    metrics.kind = static_cast<std::uint32_t>(SectionKind::Metrics);
    metrics.version = 1;
    metrics.offset = metrics_at;
    metrics.length = kRecords * sizeof(MetricRecord);
    metrics.element_size = sizeof(MetricRecord);
    metrics.count = kRecords;
    metrics.capacity = kRecords * sizeof(MetricRecord);
    metrics.generation = 2;
    metrics.fingerprint = section_layout_fingerprint(SectionKind::Metrics);
    std::memcpy(img.data() + dir + sizeof(SectionEntry), &metrics, sizeof(metrics));

    SectionEntry worldlines{};
    worldlines.kind = static_cast<std::uint32_t>(SectionKind::Worldlines);
    worldlines.version = 1;
    worldlines.offset = worldlines_at;
    worldlines.length = kRecords * sizeof(WorldlineRecord);
    worldlines.element_size = sizeof(WorldlineRecord);
    worldlines.count = kRecords;
    // The capacity is larger than the records: the plan must report
    // the room that is left.
    worldlines.capacity = 2 * kRecords * sizeof(WorldlineRecord);
    worldlines.generation = 2;
    worldlines.fingerprint = section_layout_fingerprint(SectionKind::Worldlines);
    std::memcpy(img.data() + dir, &worldlines, sizeof(worldlines));

    // Commit the digests: the reader checks them.
    const auto digest = [&](std::uint64_t at, std::uint64_t length) {
        std::uint64_t value = 14695981039346656037ull;
        for (std::uint64_t i = 0; i < length; ++i) {
            value ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[at + i]));
            value *= 1099511628211ull;
        }
        return value;
    };
    metrics.digest = digest(metrics_at, metrics.length);
    std::memcpy(img.data() + dir + sizeof(SectionEntry), &metrics, sizeof(metrics));
    worldlines.digest = digest(worldlines_at, worldlines.length);
    std::memcpy(img.data() + dir, &worldlines, sizeof(worldlines));
    return true;
}

std::string test_path() {
    return "/tmp/aura-slo-" + std::to_string(::getpid()) + ".sock";
}

// ── The std/bus surface, driven through a real aura process ────
std::string aura_binary() {
    char exe[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        const std::string self(exe, static_cast<std::size_t>(n));
        const auto slash = self.find_last_of('/');
        if (slash != std::string::npos)
            return self.substr(0, slash + 1) + "aura";
    }
    return "./aura";
}

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fputs(text.c_str(), f);
    std::fclose(f);
    return true;
}

pid_t run_script(const std::string& script, int* out_fd, int* in_fd) {
    int out[2], in[2];
    if (pipe(out) != 0 || pipe(in) != 0)
        return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(out[0]);
        ::close(in[1]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        dup2(in[0], STDIN_FILENO);
        ::close(out[1]);
        ::close(in[0]);
        setenv("AURA_SANDBOX", "off", 1);
        const std::string exe = aura_binary();
        execl(exe.c_str(), "aura", script.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    ::close(in[0]);
    *out_fd = out[0];
    *in_fd = in[1];
    return pid;
}

std::string read_until(int fd, const char* marker, int timeout_ms) {
    std::string s;
    char buf[512];
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        waited += 50;
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
            if (s.find(marker) != std::string::npos)
                break;
        }
    }
    return s;
}

bool has(const std::string& out, const char* line) {
    return out.find(line) != std::string::npos;
}

// The host publishes a few metrics and reads the counters back.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8963))
(define ms (bus:attach "" 8963 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(bus:publish msid 9 40)
(bus:publish msid 9 41)
(bus:publish msid 9 42)
(define s (bus:slo msid))
(display "slo=") (display s) (display "|") (newline)
(define c (bus:capacity-plan msid))
(display "capacity=") (display c) (display "|") (newline)
(display "DONE") (newline)
)AURA";

} // namespace

int main() {
    // ── The histogram ──────────────────────────────────────────
    {
        SloHistogram histogram;
        check(histogram.p99() == 0, "an empty histogram has no p99");
        for (int i = 0; i < 99; ++i)
            histogram.add(1000);
        histogram.add(1000000);
        check(histogram.count() == 100, "every sample counts");
        // The 99th percentile of 99 cheap samples and one expensive one
        // is still in the cheap bucket.
        check(histogram.p99() < 1000000, "the p99 ignores the one expensive sample");
        SloHistogram single;
        single.add(4096);
        check(single.p99() >= 4096, "the p99 covers the sample");
    }

    // ── The coordinator's counters and the capacity plan ────────
    {
        const std::string path = test_path();
        int ready[2];
        int result[2];
        check(pipe(ready) == 0 && pipe(result) == 0, "pipes");
        const pid_t pid = fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            ::close(ready[0]);
            ::close(result[0]);
            Server server;
            TransportError err;
            if (!server.listen_at(path, 64 * 1024, 79, &err))
                _exit(20);
            WorkspacePolicy policy;
            policy.allow(0, Capability{kCapAll, 0xFF, 79, 0, 0, 0});
            server.set_policy(policy);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(21);
            Attach peer;
            if (!server.accept_one(build_small_image, &peer, &err))
                _exit(22);
            if (!server.wait_disconnect(&err))
                _exit(23);
            const SloCounters& counters = server.slo();
            const CapacityPlan plan = server.capacity_plan();
            std::uint64_t out[6] = {counters.frames,     counters.publishes, counters.catchups,
                                    counters.p99_cpu_ns, plan.rounds,        plan.low};
            if (::write(result[1], out, sizeof(out)) != sizeof(out))
                _exit(24);
            _exit(0);
        }
        ::close(ready[1]);
        ::close(result[1]);
        char r = 0;
        check(::read(ready[0], &r, 1) == 1, "server ready");

        Attach att;
        TransportError err;
        Client client;
        check(client.connect_to(path, 79, 0, kCapAll, &att, &err), "attach");
        // The capacity plan answers a read.
        SloQueryBody request{};
        request.kind = 1;
        check(send_frame(att.sock, FrameType::SloQuery, att.session,
                         {reinterpret_cast<const std::byte*>(&request), sizeof(request)}),
              "capacity query");
        FrameHeader hdr{};
        std::vector<std::byte> body(kBusMaxControlFrame);
        check(recv_frame(att.sock, &hdr, body, &err), "capacity reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit), "the plan answers");
        SloReplyBody plan{};
        std::memcpy(&plan, body.data(), sizeof(plan));
        check(plan.rounds <= 4, "the plan counts the rounds the room supports");
        check(plan.low == 1, "the plan flags the low room");
        check(plan.remaining[3] == 4, "the plan reports the worldline room");
        att.close_all();

        std::uint64_t out[6] = {0, 0, 0, 0, 0, 0};
        check(::read(result[0], out, sizeof(out)) == static_cast<ssize_t>(sizeof(out)),
              "server counters");
        // The small image holds four worldline records, so the plan
        // knows the room is short.
        check(out[4] <= 4, "the capacity plan counts the rounds the room supports");
        check(out[5] == 1, "the capacity plan flags the low room");
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server clean exit");
        ::close(ready[0]);
        ::close(result[0]);
        ::unlink(path.c_str());
    }

    // ── The std/bus surface ────────────────────────────────────
    {
        char tmpl[] = "/tmp/aura-bus-slo-XXXXXX";
        char* dir = mkdtemp(tmpl);
        check(dir != nullptr, "temp dir");
        if (!dir)
            return 1;
        check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
        const std::string d(dir);
        check(write_file(d + "/slo.aura", kHostScript), "write the script");
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/slo.aura", &out_fd, &in_fd);
        check(pid > 0, "fork the host");
        // The host script is one-shot: it may unlink the socket and exit
        // before the first stat, so the output is the readiness signal
        // (the #5128 host is exercised by test_bus_host separately).
        const std::string out = read_until(out_fd, "DONE", 20000);
        check(has(out, "slo="), "the slo counters are readable");
        check(has(out, "capacity="), "the capacity plan is readable");
        check(has(out, "(ok "), "the records are ok");
        ::close(in_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the host finished");
    }

    if (g_failed == 0)
        std::printf("test_bus_slo: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
