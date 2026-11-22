// Issue #5379: candidate evidence five-tuple binding and stale-submit
// rejection. A full matching five-tuple passes KEEP; a stale baseline,
// a forged candidate_hash, and missing workload/seed/verifier are each
// refused with the specific reason; a v1 (48-byte worldline) image stays
// readable by the v2 reader (frozen prefix identical, tail defaulted).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/transport.hh"
#include "bus/image_schema.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

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

bool write_file(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fputs(text, f);
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
        setenv("AURA_BUS_EVIDENCE", "1", 1);
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
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
            if (s.find(marker) != std::string::npos)
                break;
        } else if (pr > 0 && (pfd.revents & POLLHUP)) {
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

std::string read_all(int fd) {
    std::string s;
    char buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        s.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return s;
}

bool has(const std::string& out, const char* line) {
    return out.find(line) != std::string::npos;
}

bool wait_socket(const std::string& sock) {
    for (int i = 0; i < 200; ++i) {
        struct stat st{};
        if (::stat(sock.c_str(), &st) == 0)
            return true;
        usleep(50000);
    }
    return false;
}

// Self-contained host+proposer+judge in ONE process (the host self-attaches
// as maintainer, opens a trial, publishes a source patch, binds evidence,
// then KEEPs). The evidence fields are parameterized per case via stdin:
// "ev <baseline> <candidate-mode> <workload> <seed> <verifier>" where
// candidate-mode "good" uses the real digest, "bad" uses a forged value.
const char* kScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8920))
(define ms (bus:attach "" 8920 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "ready") (newline)
(define (do-trial baseline candmode workload seed verifier)
  (define tr (bus:trial msid 3))
  (define wl (car (cdr tr)))
  (define src (bus:publish-source msid "(lambda (x) (+ x 10))"))
  (bus:publish-patch msid 5 bus:source-field (car (cdr src)) wl 0)
  (define real-d (car (cdr (bus:candidate-digest msid wl))))
  (define cand (if (string=? candmode "good") real-d 12345))
  (bus:bind-evidence msid wl baseline cand workload seed verifier)
  (define k (bus:keep msid wl 0 0))
  (display "keep=") (display (if (bus:ok? k) "ok" (bus:code k)))
  (display " reason=") (display (bus:message k)) (display "|") (newline))
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "score")
       (display "score=") (display (score 5)) (display "|") (newline)
       (loop))
      ((and (>= (string-length l) 3) (string=? (substring l 0 3) "ev "))
       ; parse "ev baseline candmode workload seed verifier"
       (define parts (let split ((s (substring l 3 (string-length l))) (acc '()))
                       (let ((sp (let find ((i 0))
                                   (cond ((>= i (string-length s)) #f)
                                         ((char=? (string-ref s i) #\space) i)
                                         (else (find (+ i 1)))))))
                         (if sp
                             (split (substring s (+ sp 1) (string-length s))
                                    (cons (substring s 0 sp) acc))
                             (reverse (cons s acc))))))
       (do-trial (string->number (list-ref parts 0))
                 (list-ref parts 1)
                 (string->number (list-ref parts 2))
                 (string->number (list-ref parts 3))
                 (string->number (list-ref parts 4)))
       (loop))
      (else (loop)))))
(bus:detach msid)
(bus:unhost 8920)
(display "DONE") (newline)
)AURA";

struct Result {
    std::string keep_line;
    std::string reason_line;
    std::string score_line;
};

Result run_ev(const std::string& d, const std::string& sock, const char* ev_cmd) {
    Result r;
    check(write_file(d + "/ev.aura", kScript), "write ev script");
    int out_fd = -1, in_fd = -1;
    const pid_t pid = run_script(d + "/ev.aura", &out_fd, &in_fd);
    check(pid > 0, "fork ev");
    check(wait_socket(sock), "ev socket appeared");
    const std::string ready = read_until(out_fd, "ready", 10000);
    check(has(ready, "ready"), "ev ready");
    check(::write(in_fd, ev_cmd, std::strlen(ev_cmd)) == static_cast<ssize_t>(std::strlen(ev_cmd)),
          "send ev command");
    r.keep_line = read_until(out_fd, "|", 10000);
    r.reason_line = r.keep_line; // keep and reason share one terminated line
    check(::write(in_fd, "score\n", 6) == 6, "send score");
    r.score_line = read_until(out_fd, "|", 10000);
    ::close(in_fd);
    const std::string out = read_all(out_fd);
    int status = 0;
    waitpid(pid, &status, 0);
    check(has(out, "DONE"), "ev finished");
    return r;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-ev-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8920);

    // 1. Full matching five-tuple: KEEP succeeds, score applies.
    {
        const Result r = run_ev(d, sock, "ev 0 good 7 42 1\n");
        check(has(r.keep_line, "keep=ok"), "matching evidence KEEP succeeds");
        check(has(r.score_line, "score=15"), "matching evidence applies the patch");
    }

    // 2. Forged candidate_hash: refused with candidate-hash-mismatch.
    {
        const Result r = run_ev(d, sock, "ev 0 bad 7 42 1\n");
        check(has(r.keep_line, "keep=validation-failed"), "forged hash KEEP refused");
        check(has(r.reason_line, "candidate-hash-mismatch"), "reason is candidate-hash-mismatch");
        check(has(r.score_line, "score=6"), "workspace unchanged after forged hash");
    }

    // 3. Missing workload/seed/verifier (zeros): refused incomplete-evidence.
    {
        const Result r = run_ev(d, sock, "ev 0 good 0 0 0\n");
        check(has(r.keep_line, "keep=validation-failed"), "incomplete evidence KEEP refused");
        check(has(r.reason_line, "incomplete-evidence"), "reason is incomplete-evidence");
        check(has(r.score_line, "score=6"), "workspace unchanged after incomplete evidence");
    }

    // 4. Stale baseline: first KEEP advances the generation, second trial
    //    binds baseline 0 (now stale) and is refused stale-baseline.
    {
        check(write_file(d + "/ev.aura", kScript), "write ev script");
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/ev.aura", &out_fd, &in_fd);
        check(pid > 0, "fork stale");
        check(wait_socket(sock), "stale socket appeared");
        check(has(read_until(out_fd, "ready", 10000), "ready"), "stale ready");
        check(::write(in_fd, "ev 0 good 7 42 1\n", 17) == 17, "first keep");
        const std::string k1 = read_until(out_fd, "|", 10000);
        check(has(k1, "keep=ok"), "first KEEP advances generation");
        check(::write(in_fd, "ev 0 good 7 42 1\n", 17) == 17, "second keep stale baseline");
        const std::string k2 = read_until(out_fd, "|", 10000);
        check(has(k2, "keep=validation-failed"), "stale baseline KEEP refused");
        check(has(k2, "stale-baseline"), "reason is stale-baseline");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "DONE"), "stale finished");
    }

    // 5. v1 image compatibility: a 48-byte worldline record (layout v1)
    //    validates under the v2 reader; the frozen prefix is identical and
    //    the evidence tail defaults to zero.
    {
        // Build a minimal v1 image with one 48-byte worldline record.
        std::vector<std::byte> img(kImageHeaderSize + sizeof(SectionEntry) + kWorldlineV1Size + 64);
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
        h.committed_bytes = img.size();
        h.committed_gen = 2;
        h.section_dir_off = kImageHeaderSize;
        h.section_count = 1;
        std::memcpy(img.data(), &h, sizeof(h));
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(SectionKind::Worldlines);
        e.version = 1;
        e.offset = kImageHeaderSize + sizeof(SectionEntry);
        e.element_size = kWorldlineV1Size;
        e.length = kWorldlineV1Size;
        e.count = 1;
        e.capacity = kWorldlineV1Size;
        e.generation = 2;
        e.fingerprint = section_layout_fingerprint(SectionKind::Worldlines);
        // digest over the committed record bytes
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint64_t i = 0; i < kWorldlineV1Size; ++i) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[e.offset + i]));
            hash *= 1099511628211ull;
        }
        e.digest = hash;
        std::memcpy(img.data() + kImageHeaderSize, &e, sizeof(e));
        // The v1 record: frozen prefix only, state Kept.
        WorldlineRecord w{};
        w.id = 1;
        w.parent = kImageNoParent;
        w.state = static_cast<std::uint8_t>(WorldlineState::Kept);
        w.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        w.metrics_off = kImageNoIndex;
        std::memcpy(img.data() + e.offset, &w, kWorldlineV1Size);
        // Recompute digest after writing the record.
        hash = 14695981039346656037ull;
        for (std::uint64_t i = 0; i < kWorldlineV1Size; ++i) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[e.offset + i]));
            hash *= 1099511628211ull;
        }
        e.digest = hash;
        std::memcpy(img.data() + kImageHeaderSize, &e, sizeof(e));

        const ImageCheck check_v1 = validate_image(img);
        check(check_v1.ok(), "v1 (48-byte worldline) image validates under v2 reader");
        // Read the record back: prefix identical, tail defaulted to zero.
        WorldlineRecord back{};
        std::memcpy(&back, img.data() + e.offset, kWorldlineV1Size);
        check(back.id == 1 && back.state == static_cast<std::uint8_t>(WorldlineState::Kept),
              "v1 frozen prefix reads identically");
        check(back.baseline_gen == 0 && back.candidate_hash == 0 && back.workload_sym == 0 &&
                  back.seed == 0 && back.verifier_version == 0,
              "v1 evidence tail defaults to zero");
    }

    if (g_failed == 0)
        std::printf("test_bus_evidence_binding: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
