// @category: unit
// @reason: Issue #4936 — handoff stash insert and take actually run.
//
//   AC1: insert then take returns the token and removes it
//   AC2: a second take misses
//   AC3: the 64-cap evicts the oldest hash

#include "compiler/handoff_token_stash.hh"

#include <iostream>
#include <print>
#include <string>

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

} // namespace

int main() {
    aura::compiler::HandoffTokenStash stash;
    aura::orch::HandoffToken tok;
    tok.source_fiber_id = 42;
    tok.capability_tenant_id = 7;
    const auto inserted = stash.insert(std::move(tok));
    expect(!inserted.hash.empty() && inserted.evicted.empty() && stash.size() == 1,
           "insert stores one hash");
    aura::orch::HandoffToken taken;
    expect(stash.take(inserted.hash, taken) && taken.source_fiber_id == 42 &&
               taken.capability_tenant_id == 7,
           "take returns the inserted token");
    aura::orch::HandoffToken again;
    expect(!stash.take(inserted.hash, again) && stash.size() == 0, "second take misses");

    std::string first;
    for (int i = 0; i < 65; ++i) {
        aura::orch::HandoffToken extra;
        extra.source_fiber_id = static_cast<std::uint64_t>(i + 1);
        const auto one = stash.insert(std::move(extra));
        if (i == 0)
            first = one.hash;
        if (i == 64) {
            expect(one.evicted == first && stash.size() == 64, "65th insert evicts the oldest");
            aura::orch::HandoffToken gone;
            expect(!stash.take(first, gone), "evicted hash is gone");
            aura::orch::HandoffToken last;
            expect(stash.take(one.hash, last) && last.source_fiber_id == 65,
                   "newest token is kept");
        }
    }

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
