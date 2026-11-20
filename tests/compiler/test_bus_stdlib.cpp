// Issue #5127: std/bus two-process attach/subscribe/publish/query.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/image_schema.hh"
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

template <typename T> void put(std::span<std::byte> img, std::uint64_t off, const T& v) {
    std::memcpy(img.data() + off, &v, sizeof(T));
}

bool build_channel_image(std::span<std::byte> img, std::string*) {
    constexpr std::uint32_t kSections = 4;
    constexpr std::uint64_t kPerChannel = 8;
    const std::uint64_t dir = 256;
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
    const std::uint64_t committed = (off + 7) & ~7ull;
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
    return true;
}

const char* kScript = R"((require "std/bus" all:)
(define (show x) (display x) (newline))
(define s (bus:attach "" 99 0 127 255))
(show s)
(if (bus:ok? s)
  (begin
    (define sid (car (cdr s)))
    (define sub (bus:subscribe sid 1))
    (show sub)
    (define pub (bus:publish sid 0 100))
    (show pub)
    (define q (bus:query sid 1 2 0))
    (show q)
    (define tr (bus:trial sid 1))
    (show tr)
    (define kp (bus:keep sid (car (cdr tr)) 0 5))
    (show kp)
    (define gr (bus:grant sid 1 255 0 0 0))
    (show gr)
    (define rv (bus:revoke sid (car (cdr gr))))
    (show rv)
    (define au (bus:audit sid (car (cdr tr))))
    (show au)
    (define h (bus:help))
    (show (string-length h))
    (display (if (and (bus:ok? sub) (bus:ok? pub) (bus:ok? q) (bus:ok? tr) (bus:ok? kp)
                      (bus:ok? gr) (bus:ok? rv) (bus:ok? au)
                      (> (string-length h) 100)
                      (= (car (cdr q)) 4)
                      (= (car (cdr (cdr q))) 1)
                      (>= (car (cdr au)) 2))
                 "BUS_OK" "BUS_FAIL"))
    (newline)
    (bus:detach sid))
  (begin (display "BUS_FAIL") (newline)))
)";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-stdlib-bus-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");

    const std::string script_path = std::string(dir) + "/bus.aura";
    FILE* f = std::fopen(script_path.c_str(), "w");
    check(f != nullptr, "script file");
    if (!f)
        return 1;
    std::fputs(kScript, f);
    std::fclose(f);

    int ready[2];
    check(pipe(ready) == 0, "ready pipe");
    const pid_t server = fork();
    check(server >= 0, "fork server");
    if (server == 0) {
        ::close(ready[0]);
        Server s;
        TransportError err;
        const std::string path = default_socket_path(99);
        if (!s.listen_at(path, 1u << 16, 99, &err))
            _exit(20);
        WorkspacePolicy policy;
        policy.allow(0, Capability{kCapAll, 0xFF, 99, 0, 0, 0});
        s.set_policy(policy);
        const char r = 'R';
        if (::write(ready[1], &r, 1) != 1)
            _exit(21);
        Attach peer;
        if (!s.accept_one(build_channel_image, &peer, &err))
            _exit(22);
        if (!s.wait_disconnect(&err))
            _exit(23);
        _exit(0);
    }
    ::close(ready[1]);
    char r = 0;
    check(::read(ready[0], &r, 1) == 1, "server ready");

    char exe[PATH_MAX] = {0};
    const ssize_t exe_len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string aura_path = "./aura";
    if (exe_len > 0) {
        std::string self(exe, static_cast<std::size_t>(exe_len));
        const auto slash = self.find_last_of('/');
        if (slash != std::string::npos)
            aura_path = self.substr(0, slash + 1) + "aura";
    }
    int out[2];
    check(pipe(out) == 0, "output pipe");
    const pid_t aura = fork();
    check(aura >= 0, "fork aura");
    if (aura == 0) {
        ::close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        ::close(out[1]);
        setenv("AURA_SANDBOX", "off", 1);
        execl(aura_path.c_str(), "aura", script_path.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    std::string output;
    char buf[512];
    ssize_t n = 0;
    while ((n = ::read(out[0], buf, sizeof(buf))) > 0)
        output.append(buf, static_cast<std::size_t>(n));
    ::close(out[0]);
    int status = 0;
    waitpid(aura, &status, 0);
    // The server blocks in accept if the child never attached; tear it
    // down so a failed child reports its own output.
    kill(server, SIGKILL);
    waitpid(server, &status, 0);

    check(output.find("(ok 2 0 512 0 1)") != std::string::npos, "subscribe snapshot");
    check(output.find("(ok 4 1 0 ((0 100)))") != std::string::npos,
          "query returns the published metric");
    check(output.find("BUS_OK") != std::string::npos, "script closed the loop");
    if (g_failed != 0)
        std::printf("output:\n%s\n", output.c_str());

    ::unlink(script_path.c_str());
    ::rmdir(dir);
    if (g_failed == 0)
        std::printf("OK: std/bus #5127\n");
    return g_failed == 0 ? 0 : 1;
}
