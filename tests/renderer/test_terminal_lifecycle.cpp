// test_terminal_lifecycle.cpp — Issue #1352: delete/compact + use-after-delete

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

namespace {

std::int64_t make_buf(CompilerService& cs, int w = 4, int h = 2) {
    auto id = cs.eval(std::format("(make-terminal-buffer {} {})", w, h));
    if (!id || !is_int(*id))
        return -1;
    return as_int(*id);
}

bool del_buf(CompilerService& cs, std::int64_t id) {
    auto v = cs.eval(std::format("(delete-terminal-buffer {})", id));
    return v && is_bool(*v) && as_bool(*v);
}

std::int64_t count_live(CompilerService& cs) {
    auto v = cs.eval("(terminal-buffer-count)");
    if (!v || !is_int(*v))
        return -1;
    return as_int(*v);
}

} // namespace

int main() {
    CompilerService cs;

    // Baseline: fresh count (registry may have leftovers from static prior tests in process
    // — clear via delete of what we create only; count is absolute live).
    const auto base = count_live(cs);
    CHECK(base >= 0, "terminal-buffer-count");

    // AC1: delete-terminal-buffer success / fail
    {
        auto id = make_buf(cs);
        CHECK(id >= 0, "make-terminal-buffer");
        CHECK(del_buf(cs, id), "delete-terminal-buffer true");
        auto again = cs.eval(std::format("(delete-terminal-buffer {})", id));
        CHECK(again && is_bool(*again) && !as_bool(*again), "delete twice → #f");
        auto bad = cs.eval("(delete-terminal-buffer -1)");
        CHECK(bad && is_bool(*bad) && !as_bool(*bad), "delete invalid → #f");
    }

    // AC7: use-after-delete → set-cell / present false
    {
        auto id = make_buf(cs);
        CHECK(del_buf(cs, id), "delete for UAF checks");
        auto set = cs.eval(std::format("(terminal-set-cell {} 0 0 65 1 0)", id));
        CHECK(set && is_bool(*set) && !as_bool(*set), "set-cell after delete → #f");
        auto pres = cs.eval(std::format("(terminal-present-batch {})", id));
        CHECK(pres && is_int(*pres) && as_int(*pres) == -1, "present after delete → -1");
        auto frame = cs.eval(std::format("(terminal-frame-ansi {})", id));
        CHECK(frame.has_value(), "frame-ansi after delete returns value");
    }

    // AC: make×N delete×N → live back to baseline
    {
        const auto before = count_live(cs);
        std::vector<std::int64_t> ids;
        ids.reserve(32);
        int make_ok = 0, del_ok = 0;
        for (int i = 0; i < 32; ++i) {
            auto id = make_buf(cs, 8, 4);
            if (id >= 0) {
                ++make_ok;
                ids.push_back(id);
            }
        }
        CHECK(make_ok == 32 && count_live(cs) == before + 32, "32 live after make");
        for (auto id : ids)
            if (del_buf(cs, id))
                ++del_ok;
        CHECK(del_ok == 32 && count_live(cs) == before, "live restored after 32 delete");
    }

    // AC: make, delete, make → new id different until compact (freelist empty)
    {
        auto a = make_buf(cs);
        CHECK(del_buf(cs, a), "delete a");
        auto b = make_buf(cs);
        CHECK(b != a, "without compact, new id differs from deleted (append path)");
        // compact then make should reuse a freelist slot (possibly a if still in vector)
        auto rec = cs.eval("(compact-terminal-buffers)");
        CHECK(rec && is_int(*rec) && as_int(*rec) >= 1, "compact reclaims tombstones");
        auto c = make_buf(cs);
        // c should be a reclaimed slot id (often == a if a was mid-vector hole, or lower)
        CHECK(c >= 0, "make after compact");
        // Verify reuse happened: freelist path means c equals some prior tombstone.
        // After compact, first free is typically the lowest null index.
        CHECK(c == a || c < b || c == b, "post-compact id from freelist or append");
        (void)del_buf(cs, b);
        (void)del_buf(cs, c);
    }

    // Mid-list hole: create three, delete middle, compact, next make reuses middle
    {
        auto x = make_buf(cs);
        auto y = make_buf(cs);
        auto z = make_buf(cs);
        CHECK(x >= 0 && y >= 0 && z >= 0, "three buffers");
        CHECK(del_buf(cs, y), "delete middle");
        auto set_z = cs.eval(std::format("(terminal-set-cell {} 0 0 90 2 0)", z));
        CHECK(set_z && is_bool(*set_z) && as_bool(*set_z),
              "live buffer still works after sibling delete");
        auto r = cs.eval("(compact-terminal-buffers)");
        CHECK(r && is_int(*r) && as_int(*r) >= 1, "compact middle hole");
        auto y2 = make_buf(cs);
        CHECK(y2 == y, "compact freelist reuses middle id");
        auto set_y2 = cs.eval(std::format("(terminal-set-cell {} 0 0 89 3 0)", y2));
        CHECK(set_y2 && is_bool(*set_y2) && as_bool(*set_y2), "reused id is live");
        (void)del_buf(cs, x);
        (void)del_buf(cs, y2);
        (void)del_buf(cs, z);
    }

    // Diff after delete one side
    {
        auto a = make_buf(cs);
        auto b = make_buf(cs);
        CHECK(del_buf(cs, a), "delete a for diff");
        auto d = cs.eval(std::format("(terminal-diff-update {} {})", a, b));
        CHECK(d && is_int(*d) && as_int(*d) == -1, "diff with deleted id → -1");
        (void)del_buf(cs, b);
    }

    // make+delete stress with compact (reuses slots; avoids unbounded vector growth)
    {
        const auto before = count_live(cs);
        int cycles_ok = 0;
        for (int i = 0; i < 200; ++i) {
            auto id = make_buf(cs, 16, 8);
            if (id < 0 || !del_buf(cs, id))
                break;
            ++cycles_ok;
            if ((i % 32) == 31)
                (void)cs.eval("(compact-terminal-buffers)");
        }
        (void)cs.eval("(compact-terminal-buffers)");
        CHECK(cycles_ok == 200, "200 make+delete cycles");
        CHECK(count_live(cs) == before, "make+delete leaves live count stable");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal lifecycle #1352: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
