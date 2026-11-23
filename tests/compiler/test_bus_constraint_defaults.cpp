// Issue #5401: target-allowlist and metric-floor-bound are fail-closed.
// An empty or malformed install source refuses; a valid source admits
// only the listed patch targets / values. The source-carrier marker is
// the only target-allowlist subject.

#include "bus/constraint.hh"
#include "bus/image.hh"

#include <cstdio>
#include <array>
#include <span>
#include <string>

namespace {

int g_failed = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    }
}

using namespace aura::bus;

PatchRecord source_patch(std::uint64_t node) {
    PatchRecord p{};
    p.node = node;
    p.field_offset = kPatchSourceField;
    return p;
}

} // namespace

int main() {
    std::string why;

    // target-allowlist: empty means "no source target is allowed".
    {
        const auto deny_empty = target_allowlist_predicate("");
        PatchRecord p = source_patch(11);
        ConstraintContext ctx{};
        ctx.patches = std::span<const PatchRecord>(&p, 1);
        check(!deny_empty(ctx, &why), "empty target-allowlist refuses a source patch");
        ctx.patches = std::span<const PatchRecord>();
        check(deny_empty(ctx, &why), "empty target-allowlist admits a KEEP with no source patch");
    }

    // A valid list admits listed nodes and refuses unlisted nodes.
    {
        const auto allow = target_allowlist_predicate("11, 12");
        PatchRecord p11 = source_patch(11);
        PatchRecord p13 = source_patch(13);
        ConstraintContext ctx{};
        ctx.patches = std::span<const PatchRecord>(&p11, 1);
        check(allow(ctx, &why), "listed node 11 passes");
        ctx.patches = std::span<const PatchRecord>(&p13, 1);
        check(!allow(ctx, &why), "unlisted node 13 is refused");
        PatchRecord immediate{};
        immediate.node = 99;
        immediate.field_offset = 7;
        ctx.patches = std::span<const PatchRecord>(&immediate, 1);
        check(allow(ctx, &why), "an immediate patch is not a target-allowlist subject");
    }

    // Malformed source is not a list.
    {
        const auto deny = target_allowlist_predicate("11x");
        PatchRecord p = source_patch(11);
        ConstraintContext ctx{};
        ctx.patches = std::span<const PatchRecord>(&p, 1);
        check(!deny(ctx, &why), "malformed target-allowlist source refuses");
    }

    // metric-floor-bound: no floor means refuse; a floor compares new_value.
    {
        const auto deny_empty = metric_floor_predicate("");
        ConstraintContext ctx{};
        ctx.new_value = 100;
        check(!deny_empty(ctx, &why), "empty metric floor refuses");
        const auto floor = metric_floor_predicate("10");
        ctx.new_value = 9;
        check(!floor(ctx, &why), "metric below the floor is refused");
        ctx.new_value = 10;
        check(floor(ctx, &why), "metric at the floor passes");
        ctx.new_value = 11;
        check(floor(ctx, &why), "metric above the floor passes");
    }

    // The store runs the installed rule and audits the refusal.
    {
        ConstraintSet set{};
        set.rules.push_back({"target-allowlist", target_allowlist_predicate("11"), ""});
        ConstraintStore store;
        Capability caps{};
        caps.bits = kCapConstraintWrite;
        AuditLog audit;
        check(store.install(std::move(set), caps, &audit, 1, &why), "install the allowlist rule");
        PatchRecord p11 = source_patch(11);
        PatchRecord p13 = source_patch(13);
        ConstraintContext ok{};
        ok.patches = std::span<const PatchRecord>(&p11, 1);
        check(store.check(ok, &audit, 1).pass, "store admits a listed target");
        ConstraintContext bad{};
        bad.patches = std::span<const PatchRecord>(&p13, 1);
        check(!store.check(bad, &audit, 1).pass, "store refuses an unlisted target");
    }

    // #5401: a task-scoped capability cannot open, keep or roll back
    // another task's trial.
    {
        Capability parent{};
        parent.workspace = 1;
        parent.bits = kCapAll;
        parent.channels = ~0ull;
        Capability child = parent;
        child.task = 5;
        check(attenuates(parent, child, &why), "a task scope narrows a grant");
        Capability wrong = child;
        wrong.task = 6;
        check(!attenuates(child, wrong, &why), "a grant cannot change its task scope");

        WorldlineLedger ledger;
        const std::uint64_t wl = ledger.open_trial(7, 1, 0, 5);
        Capability cap{};
        cap.workspace = 1;
        cap.bits = kCapKeep | kCapRollback;
        cap.channels = ~0ull;
        cap.task = 6;
        check(!ledger.keep(wl, 7, cap, &why), "task 6 capability cannot keep task 5");
        cap.task = 5;
        check(ledger.keep(wl, 7, cap, &why), "task 5 capability keeps task 5");
        check(!ledger.rollback(wl, 7, cap, &why), "a kept trial cannot be rolled back as pending");
    }

    if (g_failed == 0)
        std::printf("test_bus_constraint_defaults: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
