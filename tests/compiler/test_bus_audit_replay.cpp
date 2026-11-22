// Issue #5392: audit replay and deterministic reproduction.
//
// The closed loop must be provable after the fact: the audit log and
// the image must reconstruct the same lineage, every KEEP must carry
// the evidence that let it through, and the admission verdict must
// reproduce byte for byte from the bound evidence. A tampered
// candidate hash or a tampered verdict must be caught.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/gate.hh"
#include "bus/image_schema.hh"
#include "bus/replay.hh"
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

bool build_empty_image(std::span<std::byte> img, std::string*) {
    if (img.size() < kImageHeaderSize)
        return false;
    std::memset(img.data(), 0, img.size());
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
    h.layout_version = 1;
    h.committed_bytes = kImageHeaderSize;
    h.committed_gen = 2;
    h.section_dir_off = kImageHeaderSize;
    h.section_count = 0;
    std::memcpy(img.data(), &h, sizeof(h));
    return true;
}

// ── A minimal image with a worldline and an audit section ─────
struct ImageFixture {
    std::vector<std::byte> bytes;
    std::vector<WorldlineRecord> worldlines;
    std::vector<AuditRecord> audit;

    void build() {
        constexpr std::uint64_t kDir = 256;
        constexpr std::uint32_t kSections = 2;
        const std::uint64_t entries = kSections * sizeof(SectionEntry);
        const std::uint64_t worldlines_at = kDir + entries;
        const std::uint64_t audit_at = worldlines_at + worldlines.size() * sizeof(WorldlineRecord);
        const std::uint64_t committed = audit_at + audit.size() * sizeof(AuditRecord);
        bytes.assign(committed, std::byte{0});
        ImageHeader h{};
        std::memcpy(h.magic, kImageMagic, sizeof(kImageMagic));
        h.abi_major = kImageAbiMajor;
        h.abi_minor = kBusAbiMinor;
        h.endianness = static_cast<std::uint32_t>(kImageEndianMarker);
        h.header_len = kImageHeaderSize;
        h.image_size = committed;
        const SchemaId id = header_schema_id();
        std::memcpy(h.schema_id, &id.a, 8);
        std::memcpy(h.schema_id + 8, &id.b, 8);
        h.layout_version = kImageLayoutVersion;
        h.committed_bytes = committed;
        h.committed_gen = 2;
        h.section_dir_off = kDir;
        h.section_count = kSections;
        std::memcpy(bytes.data(), &h, sizeof(h));
        if (!worldlines.empty())
            std::memcpy(bytes.data() + worldlines_at, worldlines.data(),
                        worldlines.size() * sizeof(WorldlineRecord));
        if (!audit.empty())
            std::memcpy(bytes.data() + audit_at, audit.data(), audit.size() * sizeof(AuditRecord));
        const auto entry = [&](std::uint32_t index, SectionKind kind, std::uint64_t at,
                               std::uint64_t length, std::uint32_t elem, std::uint64_t count) {
            SectionEntry e{};
            e.kind = static_cast<std::uint32_t>(kind);
            e.version = 1;
            e.offset = at;
            e.length = length;
            e.element_size = elem;
            e.count = static_cast<std::uint32_t>(count);
            e.capacity = length;
            e.generation = 2;
            e.fingerprint = section_layout_fingerprint(kind);
            std::memcpy(bytes.data() + kDir +
                            static_cast<std::uint64_t>(index) * sizeof(SectionEntry),
                        &e, sizeof(e));
        };
        entry(0, SectionKind::Worldlines, worldlines_at,
              worldlines.size() * sizeof(WorldlineRecord), sizeof(WorldlineRecord),
              worldlines.size());
        entry(1, SectionKind::Audit, audit_at, audit.size() * sizeof(AuditRecord),
              sizeof(AuditRecord), audit.size());
    }

    [[nodiscard]] std::span<const std::byte> view() const { return {bytes.data(), bytes.size()}; }
};

AuditEvent event(AuditKind kind, std::uint64_t worldline, std::uint64_t timestamp_ms = 0) {
    AuditEvent e{};
    e.kind = kind;
    e.worldline = worldline;
    e.timestamp_ms = timestamp_ms;
    return e;
}

// The evidence of a candidate the coordinator admitted.
struct Accepted {
    ReplayEvidence evidence{};
    GateEvidenceBody gate{};
};

Accepted accepted_candidate(std::uint64_t worldline, std::uint64_t candidate_hash) {
    Accepted out{};
    out.gate.worldline = worldline;
    out.gate.round_id = 700;
    out.gate.workload_sym = 7;
    out.gate.seed = 42;
    out.gate.verifier_version = 3;
    out.gate.fold_count = 4;
    out.gate.control = 0;
    out.gate.verdict = 1;
    const std::uint64_t windows[4] = {500000, 600000, 700000, 800000};
    for (std::size_t i = 0; i < 4; ++i)
        out.gate.windows[i] = windows[i];
    GatePolicy policy{};
    policy.required = true;
    out.evidence.worldline = worldline;
    out.evidence.baseline_gen = 0;
    out.evidence.candidate_hash = candidate_hash;
    out.evidence.workload_sym = 7;
    out.evidence.seed = 42;
    out.evidence.verifier_version = 3;
    out.evidence.has_five = true;
    out.evidence.gate = out.gate;
    out.evidence.has_gate = true;
    out.evidence.verdict = gate_admit(out.gate, 1, policy, nullptr);
    out.evidence.has_verdict = true;
    return out;
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
        setenv("AURA_BUS_EVIDENCE", "1", 1);
        setenv("AURA_BUS_GATE", "1", 1);
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

// Run the aura CLI and capture its output: the evo reader must
// consume the exported lineage.
std::string run_cli(const std::vector<std::string>& args) {
    int out[2];
    if (pipe(out) != 0)
        return "";
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        ::close(out[1]);
        std::vector<char*> argv;
        for (const std::string& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        const std::string exe = aura_binary();
        execv(exe.c_str(), argv.data());
        _exit(25);
    }
    ::close(out[1]);
    std::string text;
    char buf[512];
    ssize_t n;
    while ((n = ::read(out[0], buf, sizeof(buf))) > 0)
        text.append(buf, static_cast<std::size_t>(n));
    ::close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return text;
}

const char* kReplayScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8942))
(define ms (bus:attach "" 8942 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "ready") (newline)
(define (open-candidate body)
  (define tr (bus:trial msid 3))
  (define wl (car (cdr tr)))
  (define src (bus:publish-source msid body))
  (bus:publish-patch msid 5 bus:source-field (car (cdr src)) wl 0)
  (define d (car (cdr (bus:candidate-digest msid wl))))
  (bus:bind-evidence msid wl 0 d 7 42 3)
  wl)
(define cwl (open-candidate "(lambda (x) (- x 10))"))
(bus:bind-gate msid cwl 700 7 42 3 3 0 1 1 (list -900000 -800000 -700000))
(define gwl (open-candidate "(lambda (x) (+ x 10))"))
(bus:bind-gate msid gwl 700 7 42 3 3 0 0 1 (list 500000 600000 700000))
(define k (bus:keep msid gwl 0 0))
(display "keep=") (display (if (bus:ok? k) "ok" (bus:code k))) (display "|") (newline)
(define r (bus:audit-replay msid))
(display "findings=") (display (list-ref r 4)) (display "|") (newline)
(display "worldlines=") (display (list-ref r 2)) (display "|") (newline)
(define e (bus:audit-evo msid))
(define text (car (cdr e)))
(display "evo=") (display (if (string-contains? text "bus:worldline") "yes" "no")) (display "|") (newline)
(display "evo2=") (display (if (string-contains? text "\"v\":2") "yes" "no")) (display "|") (newline)
(display "EVOBEGIN") (newline)
(display text)
(display "EVOEND") (newline)
(display "DONE") (newline)
)AURA";

} // namespace

int main() {
    constexpr std::uint64_t kCandidateHash = 0x5eed0000ull;

    // ── A sound lineage replays clean ──────────────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 100));
        events.push_back(event(AuditKind::GateBind, 1, 101));
        events.push_back(event(AuditKind::GateVerdict, 1, 102));
        events.push_back(event(AuditKind::Keep, 1, 103));

        ImageFixture image;
        WorldlineRecord record{};
        record.id = 1;
        record.parent = kImageNoParent;
        record.state = static_cast<std::uint8_t>(WorldlineState::Kept);
        record.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        record.metrics_off = kImageNoIndex;
        image.worldlines.push_back(record);
        AuditRecord audit{};
        audit.seq = 1;
        audit.kind = static_cast<std::uint16_t>(AuditKind::Trial);
        audit.target = 1;
        image.audit.push_back(audit);
        image.build();

        const Accepted accepted = accepted_candidate(1, kCandidateHash);
        // The published tail must agree with the verdict.
        image.worldlines[0].gate_verdict = accepted.evidence.verdict.verdict;
        image.worldlines[0].gate_family = accepted.evidence.verdict.family;
        image.worldlines[0].gate_adjusted_ppm = static_cast<std::uint64_t>(
            accepted.evidence.verdict.adjusted_ppm + static_cast<std::int64_t>(kGateBias));
        image.worldlines[0].gate_threshold_ppm = accepted.evidence.verdict.threshold_ppm;
        image.worldlines[0].gate_fingerprint = accepted.evidence.verdict.fingerprint;
        image.build();

        const std::vector<ReplayEvidence> evidence{accepted.evidence};
        const auto digest = [](std::uint64_t) { return kCandidateHash; };
        const ReplayReport report = replay_lineage(events, image.view(), evidence, true, digest);
        check(report.ok(), "a sound lineage replays clean");
        check(report.events == 4, "four events inspected");
        check(report.keeps == 1, "one keep");
        check(report.worldlines == 1, "one worldline");
        check(report.nodes.size() == 1, "one lineage node");
        check(report.nodes[0].trial_seq == 1, "the trial sequence");
        check(report.nodes[0].keep_seq == 4, "the keep sequence");
        check(report.nodes[0].verdict_seq == 3, "the verdict sequence");
        check(report.nodes[0].reproduced == 1, "the verdict reproduces byte for byte");
    }

    // ── An orphan event is found and localized ──────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Keep, 7, 10));
        const ReplayReport report = replay_lineage(events, {}, {}, true, nullptr);
        check(!report.ok(), "an orphan event is a finding");
        check(report.findings == 1, "one finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::OrphanEvent),
              "the finding names the orphan event");
        check(report.first_seq == 1, "the finding carries the sequence");
    }

    // ── A broken parent chain is found ──────────────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        ImageFixture image;
        WorldlineRecord record{};
        record.id = 1;
        record.parent = 99; // a parent that never existed
        record.state = static_cast<std::uint8_t>(WorldlineState::Kept);
        record.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        record.metrics_off = kImageNoIndex;
        image.worldlines.push_back(record);
        image.build();
        const ReplayReport report = replay_lineage(events, image.view(), {}, false, nullptr);
        check(!report.ok(), "a broken parent is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::BrokenParent),
              "the finding names the broken parent");
    }

    // ── A sequence that goes backwards is found ─────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        events.push_back(event(AuditKind::Keep, 1, 11));
        ImageFixture image;
        AuditRecord first{};
        first.seq = 1;
        first.kind = static_cast<std::uint16_t>(AuditKind::Trial);
        first.target = 1;
        AuditRecord second{};
        second.seq = 1; // the same sequence again: the mirror went backwards
        second.kind = static_cast<std::uint16_t>(AuditKind::Keep);
        second.target = 1;
        image.audit.push_back(first);
        image.audit.push_back(second);
        image.build();
        const ReplayReport report = replay_lineage(events, image.view(), {}, false, nullptr);
        check(!report.ok(), "a sequence regression is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::SeqRegression),
              "the finding names the sequence regression");
    }

    // ── The image audit and the control log must agree ─────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        ImageFixture image;
        AuditRecord mirror{};
        mirror.seq = 1;
        mirror.kind = static_cast<std::uint16_t>(AuditKind::Keep); // the log says Trial
        mirror.target = 1;
        image.audit.push_back(mirror);
        image.build();
        const ReplayReport report = replay_lineage(events, image.view(), {}, false, nullptr);
        check(!report.ok(), "a disagreeing mirror is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::AuditMismatch),
              "the finding names the audit mismatch");
    }

    // ── The reproduction is deterministic ───────────────────────
    {
        const Accepted accepted = accepted_candidate(1, kCandidateHash);
        GatePolicy policy{};
        policy.required = true;
        std::string first_reason;
        std::string second_reason;
        const GateVerdictRecord first = gate_admit(accepted.gate, 1, policy, &first_reason);
        const GateVerdictRecord second = gate_admit(accepted.gate, 1, policy, &second_reason);
        check(std::memcmp(&first, &second, sizeof(first)) == 0,
              "two runs of the adjudication agree byte for byte");
        check(std::memcmp(&first, &accepted.evidence.verdict, sizeof(first)) == 0,
              "the adjudication reproduces the recorded verdict");
    }

    // ── A tampered candidate hash is caught ─────────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        events.push_back(event(AuditKind::GateVerdict, 1, 11));
        events.push_back(event(AuditKind::Keep, 1, 12));
        const Accepted accepted = accepted_candidate(1, kCandidateHash);
        const std::vector<ReplayEvidence> evidence{accepted.evidence};
        const auto digest = [](std::uint64_t) { return kCandidateHash ^ 0x1ull; };
        const ReplayReport report = replay_lineage(events, {}, evidence, true, digest);
        check(!report.ok(), "a tampered candidate hash is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::EvidenceMismatch),
              "the finding names the evidence mismatch");
    }

    // ── A tampered verdict is caught ───────────────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        events.push_back(event(AuditKind::GateVerdict, 1, 11));
        events.push_back(event(AuditKind::Keep, 1, 12));
        Accepted accepted = accepted_candidate(1, kCandidateHash);
        accepted.evidence.verdict.adjusted_ppm += 1; // the recorded verdict was edited
        const std::vector<ReplayEvidence> evidence{accepted.evidence};
        const auto digest = [](std::uint64_t) { return kCandidateHash; };
        const ReplayReport report = replay_lineage(events, {}, evidence, true, digest);
        check(!report.ok(), "a tampered verdict is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::VerdictMismatch),
              "the finding names the verdict mismatch");
    }

    // ── A KEEP without a verdict is caught ─────────────────────
    {
        std::vector<AuditEvent> events;
        events.push_back(event(AuditKind::Trial, 1, 10));
        events.push_back(event(AuditKind::Keep, 1, 11));
        const ReplayReport report = replay_lineage(events, {}, {}, true, nullptr);
        check(!report.ok(), "a keep without a verdict is a finding");
        check(report.first_kind == static_cast<std::uint64_t>(ReplayFinding::KeepWithoutVerdict),
              "the finding names the missing verdict");
    }

    // ── The coordinator answers the replay frame ───────────────
    {
        const std::string path = "/tmp/aura-replay-" + std::to_string(::getpid()) + ".sock";
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
            if (!server.listen_at(path, 1u << 16, 78, &err))
                _exit(20);
            WorkspacePolicy policy;
            policy.allow(0, Capability{kCapAll, 0xFF, 78, 0, 0, 0});
            server.set_policy(policy);
            GatePolicy gate{};
            gate.required = true;
            server.set_gate_policy(gate);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(21);
            Attach peer;
            if (!server.accept_one(build_empty_image, &peer, &err))
                _exit(22);
            if (!server.wait_disconnect(&err))
                _exit(23);
            const ReplayReport report = server.replay();
            std::uint64_t out[4] = {report.events, report.keeps, report.findings,
                                    report.nodes.size()};
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
        check(client.connect_to(path, 78, 0, kCapAll, &att, &err), "attach");
        FrameHeader hdr{};
        std::vector<std::byte> body(kBusMaxControlFrame);
        const auto open_trial = [&]() {
            LoanBody trial{};
            trial.channel = 1;
            check(send_frame(att.sock, FrameType::Trial, att.session,
                             {reinterpret_cast<const std::byte*>(&trial), sizeof(trial)}),
                  "trial");
            check(recv_frame(att.sock, &hdr, body, &err), "trial reply");
            LoanBody granted{};
            std::memcpy(&granted, body.data(), sizeof(granted));
            return granted.worldline;
        };
        const auto bind_five = [&](std::uint64_t worldline) {
            EvidenceBody five{};
            five.worldline = worldline;
            five.baseline_gen = 0;
            five.candidate_hash = 14695981039346656037ull & ((1ull << 62) - 1);
            five.workload_sym = 7;
            five.seed = 42;
            five.verifier_version = 3;
            check(send_frame(att.sock, FrameType::EvidenceBind, att.session,
                             {reinterpret_cast<const std::byte*>(&five), sizeof(five)}),
                  "evidence bind");
            check(recv_frame(att.sock, &hdr, body, &err), "evidence bind reply");
        };
        const auto bind_gate = [&](const GateEvidenceBody& ev) {
            check(send_frame(att.sock, FrameType::GateBind, att.session,
                             {reinterpret_cast<const std::byte*>(&ev), sizeof(ev)}),
                  "gate bind");
            check(recv_frame(att.sock, &hdr, body, &err), "gate bind reply");
        };
        // The negative control of the round and the candidate that is
        // admitted: the lineage must replay clean.
        const std::uint64_t control = open_trial();
        bind_five(control);
        GateEvidenceBody control_evidence{};
        control_evidence.worldline = control;
        control_evidence.round_id = 700;
        control_evidence.workload_sym = 7;
        control_evidence.seed = 42;
        control_evidence.verifier_version = 3;
        control_evidence.fold_count = 3;
        control_evidence.control = 1;
        control_evidence.verdict = 1;
        control_evidence.windows[0] = static_cast<std::uint64_t>(-900000);
        control_evidence.windows[1] = static_cast<std::uint64_t>(-800000);
        control_evidence.windows[2] = static_cast<std::uint64_t>(-700000);
        bind_gate(control_evidence);
        const std::uint64_t admitted = open_trial();
        bind_five(admitted);
        GateEvidenceBody candidate{};
        candidate.worldline = admitted;
        candidate.round_id = 700;
        candidate.workload_sym = 7;
        candidate.seed = 42;
        candidate.verifier_version = 3;
        candidate.fold_count = 3;
        candidate.control = 0;
        candidate.verdict = 1;
        candidate.windows[0] = 500000;
        candidate.windows[1] = 600000;
        candidate.windows[2] = 700000;
        bind_gate(candidate);
        LoanBody keep{};
        keep.channel = 1;
        keep.worldline = admitted;
        check(send_frame(att.sock, FrameType::Keep, att.session,
                         {reinterpret_cast<const std::byte*>(&keep), sizeof(keep)}),
              "keep");
        check(recv_frame(att.sock, &hdr, body, &err), "keep reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit), "the keep is admitted");
        // The replay frame answers with the report.
        AuditReplayBody request{};
        check(send_frame(att.sock, FrameType::AuditReplay, att.session,
                         {reinterpret_cast<const std::byte*>(&request), sizeof(request)}),
              "audit replay");
        check(recv_frame(att.sock, &hdr, body, &err), "audit replay reply");
        check(hdr.type == static_cast<std::uint16_t>(FrameType::Credit), "the replay answers");
        check(hdr.len >= sizeof(AuditReplayReply), "the replay reply carries the report");
        AuditReplayReply reply{};
        std::memcpy(&reply, body.data(), sizeof(reply));
        check(reply.events >= 4, "the audit events are counted");
        check(reply.keeps == 1, "one keep");
        check(reply.worldlines == 2, "two worldlines in the lineage");
        check(reply.findings == 0, "the lineage replays clean");
        const std::uint64_t count = (hdr.len - sizeof(reply)) / sizeof(AuditReplayNode);
        check(count == 2, "two lineage rows");
        AuditReplayNode node{};
        std::memcpy(&node, body.data() + sizeof(reply), sizeof(node));
        check(node.worldline == control || node.worldline == admitted,
              "the row names a worldline of the round");
        check(node.trial_seq != 0, "the row carries the trial sequence");
        att.close_all();
        std::uint64_t out[4] = {0, 0, 0, 0};
        check(::read(result[0], out, sizeof(out)) == static_cast<ssize_t>(sizeof(out)),
              "server report");
        check(out[1] == 1, "the server counts the same keeps");
        check(out[2] == 0, "the server finds no defect");
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server clean exit");
        ::close(ready[0]);
        ::close(result[0]);
        ::unlink(path.c_str());
    }

    // ── The std/bus surface, end to end ─────────────────────────
    {
        char tmpl[] = "/tmp/aura-bus-replay-XXXXXX";
        char* dir = mkdtemp(tmpl);
        check(dir != nullptr, "temp dir");
        if (!dir)
            return 1;
        check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
        const std::string d(dir);
        const std::string sock = default_socket_path(8942);
        check(write_file(d + "/replay.aura", kReplayScript), "write replay script");

        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/replay.aura", &out_fd, &in_fd);
        check(pid > 0, "fork the replay host");
        check(wait_socket(sock), "the replay socket appeared");
        check(has(read_until(out_fd, "ready", 20000), "ready"), "the replay host is ready");
        const std::string out = read_until(out_fd, "DONE", 20000);
        check(has(out, "keep=ok"), "the legitimate candidate is kept");
        check(has(out, "findings=0"), "the lineage replays clean");
        check(has(out, "worldlines=2"), "the lineage holds both candidates");
        check(has(out, "evo=yes"), "the evo export carries the lineage");
        check(has(out, "evo2=yes"), "the evo export is schema v2");
        ::close(in_fd);
        const std::string rest = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(rest, "DONE") || has(out, "DONE"), "the replay host finished");

        // The evo reader consumes the exported lineage.
        const std::string begin = "EVOBEGIN\n";
        const std::string end = "\nEVOEND";
        const std::size_t from = out.find(begin);
        const std::size_t to = out.find(end);
        check(from != std::string::npos && to != std::string::npos && to > from,
              "the evo export is delimited");
        if (from != std::string::npos && to != std::string::npos && to > from) {
            const std::string jsonl = out.substr(from + begin.size(), to - from - begin.size());
            const std::string log = d + "/bus.jsonl";
            check(write_file(log, jsonl), "write the exported lineage");
            const std::string read = run_cli({"aura", "evo", "log", "--log", log});
            check(has(read, "bus:worldline"), "the evo reader consumes the export");
            check(has(read, "KEEP"), "the evo reader shows the decision");
            check(has(read, "adjusted_ppm"), "the evo reader shows the evidence");
        }
    }

    if (g_failed == 0)
        std::printf("test_bus_audit_replay: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
