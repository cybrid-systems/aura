// Issue #5124: capability bits, attenuation, grant tree, workspace
// policy and worldline ownership. No sockets here.

#include <cstdio>
#include <string>

#include "bus/capability.hh"

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

Capability make_caps(std::uint64_t bits, std::uint64_t channels, std::uint64_t workspace) {
    Capability c{};
    c.bits = bits;
    c.channels = channels;
    c.workspace = workspace;
    return c;
}

} // namespace

int main() {
    constexpr std::uint64_t ws = 42;
    constexpr std::uint64_t ch0 = 1ull << 0;
    constexpr std::uint64_t ch1 = 1ull << 1;

    // ── Attenuation ──────────────────────────────────────────────
    {
        const Capability parent =
            make_caps(kCapRead | kCapPublish | kCapTrial | kCapKeep, ch0 | ch1, ws);
        std::string why;
        Capability child = parent;
        child.bits = kCapRead | kCapPublish;
        check(attenuates(parent, child, &why), "bit subset attenuates");
        child.bits |= kCapGrant;
        check(!attenuates(parent, child, &why), "cannot add a bit");
        child = parent;
        child.channels = ch1;
        check(attenuates(parent, child, &why), "channel subset attenuates");
        child.channels = ch0 | ch1 | (1ull << 5);
        check(!attenuates(parent, child, &why), "cannot add a channel");
        child = parent;
        child.workspace = 43;
        check(!attenuates(parent, child, &why), "cannot change workspace");

        Capability bounded = parent;
        bounded.max_loans = 4;
        child = bounded;
        child.max_loans = 2;
        check(attenuates(bounded, child, &why), "quota narrows");
        child.max_loans = 8;
        check(!attenuates(bounded, child, &why), "quota cannot widen");
        child.max_loans = 0;
        check(!attenuates(bounded, child, &why), "quota cannot become unbounded");

        Capability expiring = parent;
        expiring.expires_gen = 10;
        child = expiring;
        child.expires_gen = 5;
        check(attenuates(expiring, child, &why), "expiry narrows");
        child.expires_gen = 20;
        check(!attenuates(expiring, child, &why), "expiry cannot widen");

        Capability scoped = parent;
        scoped.worldline = 7;
        child = scoped;
        child.worldline = 7;
        check(attenuates(scoped, child, &why), "same worldline");
        child.worldline = 0;
        check(!attenuates(scoped, child, &why), "cannot unscope a worldline");
    }

    // ── allows(): bits, channels, expiry, worldline ──────────────
    {
        Capability caps = make_caps(kCapRead | kCapPublish, ch0, ws);
        std::string why;
        check(allows(caps, kCapRead, 0, 2, 0, &why), "read on channel 0");
        check(!allows(caps, kCapRead, 1, 2, 0, &why), "channel 1 outside the mask");
        check(why.find("channel") != std::string::npos, "channel reason");
        check(!allows(caps, kCapKeep, 0, 2, 0, &why), "missing keep");
        check(why == "missing capability: keep", "missing keep reason");
        caps.expires_gen = 3;
        check(allows(caps, kCapRead, 0, 3, 0, &why), "at the expiry generation");
        check(!allows(caps, kCapRead, 0, 4, 0, &why), "past the expiry");
        caps.expires_gen = 0;
        caps.worldline = 9;
        // #5377: a scoped grant narrows the worldline-targeted verbs
        // (keep/rollback/trial). Reads stay allowed, so the judge can
        // read the image and the audit while it holds a scoped
        // keep+rollback on one worldline.
        check(allows(caps, kCapRead, 0, 2, 8, &why), "a scoped grant still reads");
        caps.bits |= kCapKeep;
        check(!allows(caps, kCapKeep, 0, 2, 8, &why), "a scoped keep on another worldline");
        check(why.find("scoped") != std::string::npos, "the scope reason");
        check(allows(caps, kCapRead, 0, 2, 9, &why), "matching worldline");
    }

    // ── Grant tree and revoke ───────────────────────────────────
    {
        CapabilityTable table;
        const Capability root_caps =
            make_caps(kCapRead | kCapPublish | kCapTrial | kCapKeep | kCapRollback | kCapGrant,
                      ch0 | ch1, ws);
        const std::uint64_t root = table.issue_root(root_caps);
        check(root != 0, "root token");

        Capability child_caps = root_caps;
        child_caps.bits = kCapRead | kCapGrant;
        child_caps.channels = ch0;
        std::string why;
        const auto child = table.derive(root, child_caps, &why);
        check(child.has_value(), "derive child");
        check(*child != root, "child token differs");

        Capability grand_caps = child_caps;
        grand_caps.bits = kCapRead;
        const auto grand = table.derive(*child, grand_caps, &why);
        check(grand.has_value(), "derive grandchild");

        Capability wide = root_caps;
        wide.bits |= kCapConstraintWrite;
        check(!table.derive(root, wide, &why).has_value(), "derive cannot widen");
        check(!table.derive(*grand, grand_caps, &why).has_value(), "grandchild lacks grant bit");

        Capability resolved{};
        check(table.effective(*grand, &resolved), "grandchild effective");
        check(resolved.bits == kCapRead, "grandchild bits");
        check(table.revoke(*child) == 2, "revoke child and descendant");
        check(!table.effective(*child, &resolved), "child revoked");
        check(!table.effective(*grand, &resolved), "grandchild revoked with ancestor");
        check(table.revoked(*grand), "revoked reports descendant");
        check(table.revoke(*child) == 0, "second revoke is a no-op");
    }

    // ── Workspace policy ────────────────────────────────────────
    {
        WorkspacePolicy policy;
        policy.allow(1, make_caps(kCapRead | kCapPublish, ch0 | ch1, ws));
        policy.allow(2, make_caps(kCapRead | kCapTrial | kCapKeep, ch0, ws));

        const Capability request = make_caps(kCapAll, ch0 | ch1 | (1ull << 3), ws);
        bool denied = false;
        const Capability granted = policy.resolve(1, request, ws, &denied);
        check(denied, "role 1 request narrowed");
        check(granted.bits == (kCapRead | kCapPublish), "role 1 bits");
        check(granted.channels == (ch0 | ch1), "role 1 channels");
        check(granted.workspace == ws, "grant workspace");

        const Capability unknown = policy.resolve(9, request, ws, &denied);
        check(denied && unknown.bits == 0, "unknown role is denied");
        check(unknown.workspace == ws, "denied grant keeps workspace");

        const Capability maint = policy.resolve(2, request, ws, &denied);
        check(maint.bits == (kCapRead | kCapTrial | kCapKeep), "role 2 bits");
        check(maint.channels == ch0, "role 2 channels");
    }

    // ── Preset clamp ────────────────────────────────────────────
    {
        const Capability all = make_caps(kCapAll, ch0, ws);
        check(clamp_preset(all, SandboxPreset::Dev).bits == kCapAll, "dev keeps all");
        check((clamp_preset(all, SandboxPreset::Ci).bits & kCapConstraintWrite) == 0,
              "ci drops constraint-write");
        const Capability prod = clamp_preset(all, SandboxPreset::Prod);
        check((prod.bits & (kCapGrant | kCapConstraintWrite)) == 0, "prod drops grant");
        check((prod.bits & kCapTrial) != 0, "prod keeps trial");
    }

    // ── Worldline ownership, keep/rollback, session end ─────────
    {
        WorldlineLedger ledger;
        AuditLog audit;
        const std::uint64_t alice = 100;
        const std::uint64_t bob = 200;
        const Capability alice_caps = make_caps(kCapTrial | kCapKeep | kCapRollback, ch0, ws);
        const Capability bob_caps = make_caps(kCapRead | kCapKeep, ch0, ws);
        const std::uint64_t w1 = ledger.open_trial(alice, ws);
        audit.add(AuditKind::Trial, alice, w1);
        check(w1 != 0, "trial opens");
        check(ledger.find(w1)->pending, "trial is pending");

        std::string why;
        check(!ledger.keep(w1, bob, bob_caps, &why), "bob cannot keep alice's trial");
        check(why.find("owned") != std::string::npos, "ownership reason");

        Capability scoped = alice_caps;
        scoped.bits |= kCapKeep;
        scoped.worldline = w1;
        check(ledger.keep(w1, bob, scoped, &why), "granted worldline keep succeeds");
        check(ledger.find(w1)->kept, "kept state");
        check(!ledger.keep(w1, alice, alice_caps, &why), "cannot keep twice");

        const std::uint64_t w2 = ledger.open_trial(alice, ws);
        ledger.release_loan(w2);
        check(!ledger.keep(w2, alice, alice_caps, &why), "keep needs an outstanding loan");
        check(why.find("loan") != std::string::npos, "loan reason");
        check(ledger.rollback(w2, alice, alice_caps, &why), "rollback the released trial");

        const std::uint64_t w3 = ledger.open_trial(alice, ws);
        check(ledger.rollback(w3, alice, alice_caps, &why), "owner rollback");
        check(ledger.find(w3)->rolled_back, "rolled back state");

        const std::uint64_t w4 = ledger.open_trial(alice, ws);
        const auto rolled = ledger.end_session(alice);
        check(rolled.size() == 1 && rolled[0] == w4, "session end rolls pending");
        check(ledger.find(w4)->rolled_back, "pending marked rolled");
        for (auto id : rolled)
            audit.add(AuditKind::Rollback, alice, id);
        audit.add(AuditKind::Disconnect, alice);
        check(audit.count(AuditKind::Rollback) == 1, "rollback audited");
        check(audit.count(AuditKind::Disconnect) == 1, "disconnect audited");
        check(audit.count(AuditKind::Trial) == 1, "trial audited");
    }

    if (g_failed == 0)
        std::printf("OK: bus capability #5124\n");
    return g_failed == 0 ? 0 : 1;
}
