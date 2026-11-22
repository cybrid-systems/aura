// Issue #5391: the statistical admission contract, busified.
//
// The coordinator recomputes the judge's held-out numbers from the
// bound windows, corrects them for the round family it observed
// itself, and refuses anything that does not clear the bar. A KEEP
// without statistical evidence is refused (missing-gate-evidence); a
// round without a negative control is refused; a control the
// statistics accept is a gate failure that feeds the negative-KEEP
// budget; the multiple-comparison correction bites harder as the
// family grows; and a KEEP the statistics reject never reaches the
// live workspace.

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

std::string test_path() {
    return "/tmp/aura-gate-" + std::to_string(::getpid()) + ".sock";
}

bool recv_loan(int fd, LoanBody* out, TransportError* err) {
    FrameHeader hdr{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    if (!recv_frame(fd, &hdr, body, err))
        return false;
    if (hdr.len < sizeof(*out))
        return false;
    std::memcpy(out, body.data(), sizeof(*out));
    return hdr.type == static_cast<std::uint16_t>(FrameType::Credit);
}

bool recv_error(int fd, int* code, std::string* message, TransportError* err) {
    FrameHeader hdr{};
    std::vector<std::byte> body(kBusMaxControlFrame);
    if (!recv_frame(fd, &hdr, body, err))
        return false;
    if (hdr.type != static_cast<std::uint16_t>(FrameType::Error) || hdr.len < sizeof(ErrorBody))
        return false;
    ErrorBody eb{};
    std::memcpy(&eb, body.data(), sizeof(eb));
    *code = static_cast<int>(eb.code);
    *message = eb.message;
    return true;
}

// A candidate's held-out evidence: the windows are the per-fold
// out-of-sample metric deltas in parts per million.
GateEvidenceBody make_evidence(std::uint64_t worldline, std::uint64_t round,
                               std::initializer_list<std::int64_t> windows, bool control) {
    GateEvidenceBody ev{};
    ev.worldline = worldline;
    ev.round_id = round;
    ev.workload_sym = 7;
    ev.seed = 42;
    ev.verifier_version = 3;
    ev.fold_count = windows.size();
    ev.threshold_ppm = 0;
    ev.control = control ? 1 : 0;
    ev.verdict = 1;
    std::size_t i = 0;
    for (const std::int64_t w : windows)
        ev.windows[i++] = static_cast<std::uint64_t>(w);
    return ev;
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
        // #5391: the host enforces the statistical admission.
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

// The host, the proposer and the judge in one process: the judge binds
// the held-out evidence of a negative control and of a legitimate
// candidate, the legitimate candidate is KEPT and the coordinator's own
// verdict is read back.
const char* kGateScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8941))
(define ms (bus:attach "" 8941 3 127 255))
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
; The negative control: a known-harmful candidate the statistics must
; reject. It is bound, never applied.
(define cwl (open-candidate "(lambda (x) (- x 10))"))
(define cg (bus:bind-gate msid cwl 700 7 42 3 3 0 1 1 (list -900000 -800000 -700000)))
(display "bind=") (display (if (bus:ok? cg) "ok" (bus:code cg))) (display "|") (newline)
; The legitimate candidate of the same round.
(define gwl (open-candidate "(lambda (x) (+ x 10))"))
(define gg (bus:bind-gate msid gwl 700 7 42 3 3 0 0 1 (list 500000 600000 700000)))
(display "bind2=") (display (if (bus:ok? gg) "ok" (bus:code gg))) (display "|") (newline)
(define k (bus:keep msid gwl 0 0))
(display "keep=") (display (if (bus:ok? k) "ok" (bus:code k))) (display "|") (newline)
; The negative control is never applied, even though its own numbers
; would allow it.
(define ck (bus:keep msid cwl 0 0))
(display "keepc=") (display (if (bus:ok? ck) "ok" (bus:code ck))) (display "|") (newline)
(define gv (bus:gate-evidence msid gwl))
(display "verdict=") (display (car (cdr gv))) (display "|") (newline)
(display "score=") (display (score 5)) (display "|") (newline)
(display "DONE") (newline)
)AURA";

} // namespace

int main() {
    // ── The contract itself ─────────────────────────────────────
    {
        // The window statistics: the mean and the standard error of
        // the held-out windows.
        const std::uint64_t windows[4] = {1000000, 2000000, 3000000, 4000000};
        std::int64_t observed = 0;
        std::uint64_t se = 0;
        check(gate_window_stats(windows, 4, &observed, &se), "window stats");
        check(observed == 2500000, "the observed mean is the window mean");
        check(se == 645497, "the standard error is sd / sqrt(n)");

        // The deflation factor grows with the family: one try is
        // uncorrected by construction.
        check(gate_z_expected_max(1) == 0.0, "one try is uncorrected");
        check(gate_z_expected_max(4) > gate_z_expected_max(2), "the factor grows with the family");
        check(gate_z_expected_max(4) > 1.0 && gate_z_expected_max(4) < 1.1,
              "the factor matches the standard expected maximum");

        GatePolicy policy{};
        policy.required = true;
        policy.min_windows = 3;

        // A candidate that clearly improves is admitted.
        const GateEvidenceBody good =
            make_evidence(1, 100, {500000, 600000, 700000, 800000}, false);
        std::string reason;
        const GateVerdictRecord keep = gate_admit(good, 1, policy, &reason);
        check(keep.verdict == static_cast<std::uint64_t>(GateVerdict::Keep), "a clear win is kept");
        check(reason == "ok", "the verdict names its reason");

        // A harmful candidate is rejected: its held-out mean is below
        // zero, so no haircut can save it.
        const GateEvidenceBody harmful =
            make_evidence(2, 100, {-500000, -400000, -600000, -300000}, false);
        const GateVerdictRecord reject = gate_admit(harmful, 1, policy, &reason);
        check(reject.verdict == static_cast<std::uint64_t>(GateVerdict::Reject),
              "a harmful candidate is rejected");
        check(reason == "not-better", "the rejection names the reason");

        // Too few held-out windows is not evidence.
        const GateEvidenceBody thin = make_evidence(3, 100, {500000, 600000}, false);
        const GateVerdictRecord thin_rec = gate_admit(thin, 1, policy, &reason);
        check(thin_rec.verdict == static_cast<std::uint64_t>(GateVerdict::Reject),
              "too few windows is rejected");
        check(reason == "insufficient-windows", "the rejection names the window count");

        // The multiple-comparison correction: the same candidate that
        // clears the bar alone does not clear it inside a family of
        // eight candidates.
        const GateEvidenceBody marginal = make_evidence(4, 100, {0, 20000, -10000, 10000}, false);
        const GateVerdictRecord alone = gate_admit(marginal, 1, policy, &reason);
        const GateVerdictRecord family = gate_admit(marginal, 8, policy, &reason);
        check(alone.verdict == static_cast<std::uint64_t>(GateVerdict::Keep),
              "a marginal candidate passes alone");
        check(family.verdict == static_cast<std::uint64_t>(GateVerdict::Reject),
              "the family correction rejects what a single try accepted");
        check(family.family == 8, "the verdict records the family it judged");

        // The verdict is deterministic: the same evidence twice yields
        // the very same record, byte for byte.
        const GateVerdictRecord again = gate_admit(marginal, 8, policy, &reason);
        check(std::memcmp(&again, &family, sizeof(family)) == 0,
              "the same evidence reproduces the verdict byte for byte");

        // The fingerprint binds the windows: a different window set is
        // a different evidence.
        const GateEvidenceBody tampered =
            make_evidence(4, 100, {300000, 320000, 280000, 999999}, false);
        check(gate_fingerprint(tampered) != gate_fingerprint(marginal),
              "the fingerprint binds the windows");

        // The negative-KEEP window: the rate and the budget.
        NegativeKeepWindow window;
        window.configure(4);
        check(window.total() == 0 && window.rate_ppm() == 0, "an empty window has no rate");
        window.record_positive();
        window.record_positive();
        window.record_positive();
        const std::size_t slot = window.record_positive();
        window.mark_negative(slot);
        check(window.negatives() == 1, "the window counts the negative event");
        check(window.rate_ppm() == 250000, "the rate is the negative share");
        check(window.full(), "the window is full");
        check(window.over_budget(200000), "the rate is over a 20% budget");
        check(!window.over_budget(300000), "the rate is under a 30% budget");
    }

    // ── The coordinator enforces the contract ───────────────────
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
            if (!server.listen_at(path, 1u << 16, 77, &err))
                _exit(20);
            WorkspacePolicy policy;
            policy.allow(0, Capability{kCapAll, 0xFF, 77, 0, 0, 0});
            server.set_policy(policy);
            server.set_evidence_required(true);
            GatePolicy gate{};
            gate.required = true;
            gate.budget_window = 1;
            gate.negative_budget_ppm = 0; // any negative event is over budget
            server.set_gate_policy(gate);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(21);
            Attach peer;
            if (!server.accept_one(build_empty_image, &peer, &err))
                _exit(22);
            if (!server.wait_disconnect(&err))
                _exit(23);
            // The verdict of the worldline that was admitted (the
            // fourth trial: the legitimate candidate of round 300).
            const GateVerdictRecord* verdict = server.gate_verdict(4);
            std::uint64_t out[6] = {0, 0, 0, 0, 0, 0};
            out[0] = server.audit().count(AuditKind::GateBind);
            out[1] = server.audit().count(AuditKind::GateVerdict);
            out[2] = server.audit().count(AuditKind::GateTighten);
            out[3] = server.audit().count(AuditKind::Keep);
            out[4] = server.gate_policy().tighten_factor;
            out[5] = verdict != nullptr ? verdict->verdict : 0;
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
        check(client.connect_to(path, 77, 0, kCapAll, &att, &err), "attach");

        auto open_trial = [&]() {
            LoanBody trial{};
            trial.channel = 1;
            check(send_frame(att.sock, FrameType::Trial, att.session,
                             {reinterpret_cast<const std::byte*>(&trial), sizeof(trial)}),
                  "trial");
            LoanBody granted{};
            check(recv_loan(att.sock, &granted, &err), "trial grant");
            return granted.worldline;
        };
        // The five-tuple the gate evidence must agree with. The trial
        // has no patches, so the candidate digest is the empty digest.
        auto bind_five = [&](std::uint64_t worldline) {
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
            LoanBody ok{};
            check(recv_loan(att.sock, &ok, &err), "evidence bind reply");
        };
        auto bind_gate = [&](const GateEvidenceBody& ev) {
            check(send_frame(att.sock, FrameType::GateBind, att.session,
                             {reinterpret_cast<const std::byte*>(&ev), sizeof(ev)}),
                  "gate bind");
            LoanBody ok{};
            check(recv_loan(att.sock, &ok, &err), "gate bind reply");
        };
        auto send_keep = [&](std::uint64_t worldline) {
            LoanBody keep{};
            keep.channel = 1;
            keep.worldline = worldline;
            check(send_frame(att.sock, FrameType::Keep, att.session,
                             {reinterpret_cast<const std::byte*>(&keep), sizeof(keep)}),
                  "keep");
        };

        // 1. A KEEP without statistical evidence is refused.
        {
            const std::uint64_t wl = open_trial();
            bind_five(wl);
            send_keep(wl);
            int code = 0;
            std::string message;
            check(recv_error(att.sock, &code, &message, &err), "keep refused");
            check(code == EINVAL && message.find("missing-gate-evidence") != std::string::npos,
                  "the refusal names the missing statistical evidence");
        }

        // 2. A round without a negative control is refused.
        {
            const std::uint64_t wl = open_trial();
            bind_five(wl);
            bind_gate(make_evidence(wl, 200, {500000, 600000, 700000}, false));
            send_keep(wl);
            int code = 0;
            std::string message;
            check(recv_error(att.sock, &code, &message, &err), "keep refused");
            check(code == EINVAL && message.find("missing-negative-control") != std::string::npos,
                  "the refusal names the missing negative control");
        }

        // 3. A round with a control the statistics reject: the harmful
        //    candidate is refused and the control is never applied.
        {
            const std::uint64_t control = open_trial();
            bind_five(control);
            bind_gate(make_evidence(control, 300, {-900000, -800000, -700000}, true));
            const std::uint64_t wl = open_trial();
            bind_five(wl);
            bind_gate(make_evidence(wl, 300, {500000, 600000, 700000}, false));
            // The control is never applied, even though its own
            // statistics would allow it.
            send_keep(control);
            int code = 0;
            std::string message;
            check(recv_error(att.sock, &code, &message, &err), "control keep refused");
            check(code == EINVAL && message.find("negative-control-refused") != std::string::npos,
                  "the refusal names the negative control");
            // The harmful candidate of the round is refused by the
            // statistics.
            const std::uint64_t harmful = open_trial();
            bind_five(harmful);
            bind_gate(make_evidence(harmful, 300, {-900000, -800000, -700000}, false));
            send_keep(harmful);
            check(recv_error(att.sock, &code, &message, &err), "harmful keep refused");
            check(code == EINVAL && message.find("gate-rejected") != std::string::npos,
                  "the refusal names the statistical rejection");
            // The legitimate candidate of the same round is admitted.
            send_keep(wl);
            LoanBody ok{};
            check(recv_loan(att.sock, &ok, &err), "the legitimate candidate is kept");
        }

        // 4. A control the statistics accept is a gate failure: the
        //    round is refused and the negative-KEEP budget tightens
        //    the admission.
        {
            const std::uint64_t control = open_trial();
            bind_five(control);
            bind_gate(make_evidence(control, 400, {900000, 800000, 700000}, true));
            const std::uint64_t wl = open_trial();
            bind_five(wl);
            bind_gate(make_evidence(wl, 400, {500000, 600000, 700000}, false));
            send_keep(wl);
            int code = 0;
            std::string message;
            check(recv_error(att.sock, &code, &message, &err), "keep refused");
            check(code == EINVAL && message.find("negative-control-accepted") != std::string::npos,
                  "the refusal names the accepted control");
        }

        // 5. The multiple-comparison correction on the wire: the same
        //    candidate that passes alone is refused inside a family.
        {
            const std::uint64_t control = open_trial();
            bind_five(control);
            bind_gate(make_evidence(control, 500, {-900000, -800000, -700000}, true));
            // Six more candidates join the family before the marginal
            // one is judged.
            for (int i = 0; i < 6; ++i) {
                const std::uint64_t other = open_trial();
                bind_five(other);
                bind_gate(make_evidence(other, 500, {400000, 410000, 390000}, false));
            }
            const std::uint64_t wl = open_trial();
            bind_five(wl);
            bind_gate(make_evidence(wl, 500, {0, 20000, -10000}, false));
            send_keep(wl);
            int code = 0;
            std::string message;
            check(recv_error(att.sock, &code, &message, &err), "keep refused");
            check(code == EINVAL && message.find("gate-rejected") != std::string::npos,
                  "the family correction refuses the marginal candidate");
        }

        att.close_all();
        std::uint64_t out[6] = {0, 0, 0, 0, 0, 0};
        check(::read(result[0], out, sizeof(out)) == static_cast<ssize_t>(sizeof(out)),
              "server audit counts");
        check(out[0] == 14, "fourteen gate evidence bindings");
        check(out[1] >= 4, "the admission verdicts are audited");
        check(out[2] == 1, "the tightened admission is audited once");
        check(out[3] == 1, "exactly one KEEP was committed");
        check(out[4] == 2, "the admission tightened once (the haircut factor doubled)");
        check(out[5] == static_cast<std::uint64_t>(GateVerdict::Keep),
              "the admitted worldline carries a keep verdict");
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server clean exit");
        ::close(ready[0]);
        ::close(result[0]);
        ::unlink(path.c_str());
    }

    // ── The layout stays additive ───────────────────────────────
    {
        // A v2 image (88-byte worldline records) still validates under
        // the v3 reader, and the gate verdict tail defaults to zero.
        const std::uint32_t v2_size = 88;
        std::vector<std::byte> img(kImageHeaderSize + sizeof(SectionEntry) + v2_size + 64);
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
        h.layout_version = 2;
        h.committed_bytes = img.size();
        h.committed_gen = 2;
        h.section_dir_off = kImageHeaderSize;
        h.section_count = 1;
        std::memcpy(img.data(), &h, sizeof(h));
        SectionEntry e{};
        e.kind = static_cast<std::uint32_t>(SectionKind::Worldlines);
        e.version = 1;
        e.offset = kImageHeaderSize + sizeof(SectionEntry);
        e.element_size = v2_size;
        e.length = v2_size;
        e.count = 1;
        e.capacity = v2_size;
        e.generation = 2;
        e.fingerprint = section_layout_fingerprint(SectionKind::Worldlines);
        WorldlineRecord w{};
        w.id = 1;
        w.parent = kImageNoParent;
        w.state = static_cast<std::uint8_t>(WorldlineState::Kept);
        w.constraint_result = static_cast<std::uint8_t>(ConstraintResult::Pass);
        w.metrics_off = kImageNoIndex;
        w.baseline_gen = 2;
        w.candidate_hash = 0x5eed0000;
        w.workload_sym = 7;
        w.seed = 42;
        w.verifier_version = 1;
        std::memcpy(img.data() + e.offset, &w, v2_size);
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint64_t i = 0; i < v2_size; ++i) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(img[e.offset + i]));
            hash *= 1099511628211ull;
        }
        e.digest = hash;
        std::memcpy(img.data() + kImageHeaderSize, &e, sizeof(e));

        const ImageCheck image_check = validate_image(img);
        check(image_check.ok(), "a v2 image validates under the v3 reader");
        WorldlineRecord back{};
        std::memcpy(&back, img.data() + e.offset, v2_size);
        check(back.id == 1 && back.state == static_cast<std::uint8_t>(WorldlineState::Kept),
              "the v2 frozen prefix reads identically");
        check(back.candidate_hash == 0x5eed0000 && back.seed == 42,
              "the v2 evidence tail reads identically");
        check(back.gate_verdict == 0 && back.gate_family == 0 && back.gate_fingerprint == 0,
              "the v3 gate tail defaults to zero");
    }

    // ── The std/bus surface, end to end ─────────────────────────
    {
        char tmpl[] = "/tmp/aura-bus-gate-XXXXXX";
        char* dir = mkdtemp(tmpl);
        check(dir != nullptr, "temp dir");
        if (!dir)
            return 1;
        check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
        const std::string d(dir);
        const std::string sock = default_socket_path(8941);
        check(write_file(d + "/gate.aura", kGateScript), "write gate script");

        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/gate.aura", &out_fd, &in_fd);
        check(pid > 0, "fork the gate host");
        check(wait_socket(sock), "the gate socket appeared");
        check(has(read_until(out_fd, "ready", 20000), "ready"), "the gate host is ready");
        const std::string out = read_until(out_fd, "DONE", 20000);
        check(has(out, "bind=ok"), "the control evidence binds");
        check(has(out, "bind2=ok"), "the candidate evidence binds");
        check(has(out, "keep=ok"), "the legitimate candidate is kept");
        check(has(out, "keepc=validation-failed"), "the negative control is refused");
        check(has(out, "verdict=1"), "the coordinator's verdict is a keep");
        check(has(out, "score=15"), "the candidate reached the live workspace");
        ::close(in_fd);
        const std::string rest = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(rest, "DONE") || has(out, "DONE"), "the gate host finished");
    }

    if (g_failed == 0)
        std::printf("test_bus_stat_gate: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
