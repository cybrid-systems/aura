#pragma once

// Shared issue-bundle driver (fork-isolated member runner).
// Profile-specific mains only declare externs + a member table, then call
// aura_run_issue_bundle(). See scripts/gen_issue_bundles.py.

struct AuraBundleMember {
    const char* name;
    int (*run)();
};

// Run every member in a forked child (crash isolation). Returns 0 if all pass.
int aura_run_issue_bundle(const char* profile, const AuraBundleMember* members, int n);
