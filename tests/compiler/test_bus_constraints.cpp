// Issue #5126: constraint gate on KEEP, versioned sets, audit.

#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "bus/constraint.hh"
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
    h.committed_bytes = kImageHeaderSize;
    h.committed_gen = 2;
    h.section_dir_off = kImageHeaderSize;
    h.section_count = 0;
    std::memcpy(img.data(), &h, sizeof(h));
    return true;
}

ConstraintSet make_set() {
    ConstraintSet set;
    set.rules.push_back({"max-new", [](const ConstraintContext& ctx, std::string* why) {
                             if (ctx.new_value > 10) {
                                 if (why)
                                     *why = "new value exceeds 10";
                                 return false;
                             }
                             return true;
                         }});
    return set;
}

std::string test_path() {
    return "/tmp/aura-con-" + std::to_string(::getpid()) + ".sock";
}

bool send_keep(int fd, std::uint64_t session, std::uint64_t worldline, std::uint64_t value) {
    LoanBody keep{};
    keep.channel = 1;
    keep.worldline = worldline;
    keep.offset = 0; // node id for the gate context
    keep.length = value;
    return send_frame(fd, FrameType::Keep, session,
                      {reinterpret_cast<const std::byte*>(&keep), sizeof(keep)});
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

} // namespace

int main() {
    constexpr std::uint64_t ws = 88;
    // ── ConstraintStore unit ────────────────────────────────────
    {
        ConstraintStore store;
        AuditLog audit;
        const ConstraintContext ctx{1, 0, 20};
        check(store.check(ctx, &audit, 1).pass, "empty set passes");
        check(audit.count(AuditKind::ConstraintResult) == 1, "pass audited");

        Capability no_write{};
        no_write.bits = kCapRead;
        no_write.workspace = ws;
        check(!store.install(make_set(), no_write, &audit, 1, nullptr),
              "install needs constraint-write");

        Capability writer{};
        writer.bits = kCapConstraintWrite;
        writer.workspace = ws;
        std::string why;
        check(store.install(make_set(), writer, &audit, 1, &why), "install with capability");
        check(store.version() == 2 && store.rule_count() == 1, "version 2 with one rule");

        const GateResult pass = store.check({1, 0, 5}, &audit, 1);
        check(pass.pass, "value 5 passes");
        const GateResult fail = store.check({1, 0, 20}, &audit, 1);
        check(!fail.pass, "value 20 fails");
        check(fail.reason.find("max-new") != std::string::npos, "failure names the rule");
        check(fail.version == 2, "failure carries the constraint version");

        check(!store.rollback(no_write, &audit, 1, &why), "rollback needs constraint-write");
        check(store.rollback(writer, &audit, 1, &why), "rollback with capability");
        check(store.version() == 1, "rollback restores version 1");
        check(store.check({1, 0, 20}, &audit, 1).pass, "empty set passes again");

        const auto events = audit.for_worldline(1);
        check(!events.empty(), "audit query by worldline");
        bool saw_reason = false;
        for (const auto& e : events) {
            if (e.kind == AuditKind::ConstraintResult && e.result == 0)
                saw_reason = e.reason.find("max-new") != std::string::npos;
        }
        check(saw_reason, "audit stores the failure reason");
        check(audit.count(AuditKind::ConstraintChange) == 1, "install audited");
        check(audit.count(AuditKind::ConstraintRollback) == 1, "rollback audited");
    }

    // ── Transport: KEEP gate and no bypass ─────────────────────
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
            if (!server.listen_at(path, 1u << 16, ws, &err))
                _exit(20);
            WorkspacePolicy policy;
            policy.allow(0, Capability{kCapAll & ~kCapConstraintWrite, 0xFF, ws, 0, 0, 0});
            server.set_policy(policy);
            std::string why;
            if (!server.install_constraints(make_set(), &why))
                _exit(21);
            const char r = 'R';
            if (::write(ready[1], &r, 1) != 1)
                _exit(22);
            Attach peer;
            if (!server.accept_one(build_empty_image, &peer, &err))
                _exit(23);
            if (!server.wait_disconnect(&err))
                _exit(24);
            std::uint64_t pass_results = 0;
            std::uint64_t fail_results = 0;
            for (const auto& e : server.audit().events()) {
                if (e.kind == AuditKind::ConstraintResult)
                    (e.result ? pass_results : fail_results) += 1;
            }
            std::uint64_t out[4] = {pass_results, fail_results,
                                    server.audit().count(AuditKind::Keep),
                                    server.audit().count(AuditKind::Rollback)};
            if (::write(result[1], out, sizeof(out)) != sizeof(out))
                _exit(25);
            _exit(0);
        }
        ::close(ready[1]);
        ::close(result[1]);
        char r = 0;
        check(::read(ready[0], &r, 1) == 1, "server ready");

        Attach att;
        TransportError err;
        Client client;
        check(client.connect_to(path, ws, 0, kCapAll, &att, &err), "attach");
        check((att.caps & kCapConstraintWrite) == 0, "constraint-write withheld");

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

        const std::uint64_t w1 = open_trial();
        check(w1 != 0, "worldline 1");
        check(send_keep(att.sock, att.session, w1, 5), "keep value 5");
        LoanBody ok{};
        check(recv_loan(att.sock, &ok, &err), "keep passes the gate");

        const std::uint64_t w2 = open_trial();
        check(send_keep(att.sock, att.session, w2, 20), "keep value 20");
        int code = 0;
        std::string message;
        check(recv_error(att.sock, &code, &message, &err), "keep rejected");
        check(code == EPERM && message.find("constraint") != std::string::npos,
              "rejection names the constraint gate");

        // No bypass: rollback needs constraint-write.
        check(send_frame(att.sock, FrameType::ConstraintRollback, att.session, {}),
              "constraint rollback");
        check(recv_error(att.sock, &code, &message, &err), "rollback rejected");
        check(code == EPERM && message.find("constraint-write") != std::string::npos,
              "rollback names the missing capability");

        att.close_all();
        std::uint64_t out[4] = {0, 0, 0, 0};
        check(::read(result[0], out, sizeof(out)) == static_cast<ssize_t>(sizeof(out)),
              "server audit counts");
        check(out[0] == 1, "one passing gate");
        check(out[1] == 1, "one failing gate");
        check(out[2] == 1, "one keep committed");
        check(out[3] == 1, "failed trial rolled back on disconnect");
        int status = 0;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "server clean exit");
        ::close(ready[0]);
        ::close(result[0]);
        ::unlink(path.c_str());
    }

    if (g_failed == 0)
        std::printf("OK: bus constraints #5126\n");
    return g_failed == 0 ? 0 : 1;
}
