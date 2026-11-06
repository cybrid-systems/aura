// @category: unit
// @reason: Issue #4965 — display/write list spine must tenant-check every
//          car the printer will emit, including index >= 64.
//
// Invokes the registered primitive through primitives().lookup. That is the
// primitive body (io_walk_tenant_allows + io_print_val), not eval dispatch
// and not a copied helper.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <utility>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_error_idx;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;
using aura::compiler::types::make_void;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::SecurityEventKind;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint64_t kTenantA = 49651;
constexpr std::uint64_t kTenantB = 49652;
constexpr const char* kSecret = "SECRET4965-spine";
constexpr const char* kOwn = "OWN4965-spine";

// A pipe fills and the writer blocks if the reader starts only after fn()
// returns. A temporary file grows past that limit. ok==false is a failed
// capture, never proof of an empty stdout.
struct StdoutCapture {
    bool ok = false;
    std::string out;
};

template <typename F> StdoutCapture capture_stdout(F&& fn) {
    StdoutCapture cap;
    std::fflush(stdout);
    const int saved = ::dup(STDOUT_FILENO);
    if (saved < 0)
        return cap;
    FILE* tmp = std::tmpfile();
    if (tmp == nullptr) {
        ::close(saved);
        return cap;
    }
    const int fd = ::fileno(tmp);
    if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) {
        std::fclose(tmp);
        ::close(saved);
        return cap;
    }
    fn();
    std::fflush(stdout);
    if (::dup2(saved, STDOUT_FILENO) < 0) {
        std::fclose(tmp);
        ::close(saved);
        return cap;
    }
    ::close(saved);
    if (std::fseek(tmp, 0, SEEK_SET) != 0) {
        std::fclose(tmp);
        return cap;
    }
    char buf[4096];
    while (true) {
        const auto n = std::fread(buf, 1, sizeof buf, tmp);
        if (n > 0)
            cap.out.append(buf, n);
        if (n < sizeof buf)
            break;
    }
    if (std::ferror(tmp)) {
        std::fclose(tmp);
        return cap;
    }
    std::fclose(tmp);
    cap.ok = true;
    return cap;
}

void arm_face(const char* sandbox, bool multi_tenant) {
    if (sandbox != nullptr && sandbox[0] != '\0')
        ::setenv("AURA_SANDBOX", sandbox, 1);
    else
        ::unsetenv("AURA_SANDBOX");
    if (multi_tenant)
        ::setenv("AURA_MULTI_TENANT", "1", 1);
    else
        ::unsetenv("AURA_MULTI_TENANT");
    aura::compiler::security::apply_production_security_defaults();
}

std::string error_text(Evaluator& ev, const EvalValue& v) {
    if (!is_error(v))
        return {};
    const auto eidx = static_cast<std::size_t>(as_error_idx(v));
    if (eidx >= ev.error_values_.size() || !is_string(ev.error_values_[eidx]))
        return {};
    return ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(ev.error_values_[eidx])));
}

struct DenyRow {
    bool found = false;
    std::uint64_t tenant = 0;
    std::uint64_t mid = 0;
    std::uint64_t epoch = 0;
    std::int64_t fiber = 0;
    std::string reason;
    int count = 0;
};

DenyRow isolation_rows_since(std::uint64_t seq_base, std::string_view op) {
    DenyRow row;
    const auto& ring = g_security_event_ring();
    const auto end = ring.seq.load(std::memory_order_acquire);
    for (std::uint64_t s = seq_base; s < end; ++s) {
        const auto& e = ring.ring[s % ring.ring.size()];
        if (e.seq != s)
            continue;
        if (e.kind != SecurityEventKind::IsolationDeny)
            continue;
        if (std::string_view(e.op) != op)
            continue;
        ++row.count;
        if (row.found)
            continue;
        row.found = true;
        row.tenant = e.tenant_id;
        row.mid = e.mutation_id;
        row.epoch = e.epoch;
        row.fiber = e.fiber_id;
        row.reason = e.reason;
    }
    return row;
}

using Prim = decltype(std::declval<Evaluator&>().primitives().lookup("display"));

EvalValue apply1(const Prim& fn, EvalValue arg) {
    if (!fn)
        return make_void();
    const EvalValue args[1] = {arg};
    return (*fn)(std::span<const EvalValue>(args, 1));
}

EvalValue apply2(const Prim& fn, EvalValue a, EvalValue b) {
    if (!fn)
        return make_void();
    const EvalValue args[2] = {a, b};
    return (*fn)(std::span<const EvalValue>(args, 2));
}

EvalValue cons_pair(const Prim& cons, EvalValue car, EvalValue cdr) {
    return apply2(cons, car, cdr);
}

// B-owned proper list. `at` < 0 means every car is an int. Otherwise that
// index holds `placed` and the other cars are distinct ints.
EvalValue proper_list(const Prim& cons, EvalValue placed, int at, int n) {
    EvalValue tail = make_void();
    for (int i = n - 1; i >= 0; --i) {
        const EvalValue car = (i == at) ? placed : make_int(i + 1);
        tail = cons_pair(cons, car, tail);
        if (!is_pair(tail))
            return make_void();
    }
    return tail;
}

EvalValue nest_singleton(const Prim& cons, EvalValue inner, int levels) {
    EvalValue v = inner;
    for (int i = 0; i < levels; ++i) {
        v = cons_pair(cons, v, make_void());
        if (!is_pair(v))
            return make_void();
    }
    return v;
}

void expect_deny_empty(Evaluator& ev, const Prim& fn, EvalValue arg, std::string_view op,
                       std::string_view label) {
    aura::core::bump_mutation_epoch(1);
    const auto epoch = aura::core::current_mutation_epoch();
    const auto fiber = static_cast<std::int64_t>(aura::core::capability::effect_fiber_id_or(
        static_cast<std::uint32_t>(aura_fiber_current_id())));
    const auto base = g_security_event_ring().seq.load(std::memory_order_acquire);
    EvalValue shown = make_void();
    const auto cap = capture_stdout([&] { shown = apply1(fn, arg); });
    const auto msg = error_text(ev, shown);
    const auto row = isolation_rows_since(base, op);
    const std::string tag(label);
    CHECK(cap.ok, tag + ": stdout capture succeeded");
    if (!cap.ok) {
        std::println(stderr, "{}: capture failed; empty is not a deny proof", tag);
        return;
    }
    if (!cap.out.empty()) {
        const auto head = cap.out.substr(0, 180);
        const auto tail = cap.out.size() > 180 ? cap.out.substr(cap.out.size() - 180) : cap.out;
        std::println(stderr, "{}: stdout bytes={} head=[{}] tail=[{}]", tag, cap.out.size(), head,
                     tail);
    }
    CHECK(cap.out.empty(), tag + ": stdout empty");
    CHECK(cap.out.find(kSecret) == std::string::npos, tag + ": secret absent");
    CHECK(is_error(shown), tag + ": primitive error");
    CHECK(msg.find("cross-tenant payload read denied (#4505)") != std::string::npos,
          tag + ": stable deny reason");
    CHECK(row.found && row.count == 1, tag + ": one IsolationDeny");
    CHECK(row.tenant == kTenantB, tag + ": IsolationDeny tenant is B");
    CHECK(row.epoch == epoch, tag + ": IsolationDeny Mutation epoch");
    CHECK(row.fiber == fiber, tag + ": IsolationDeny fiber");
    // epoch column is the emit-time Mutation epoch. mid is the deny join
    // key (TypedMid, else that epoch). A later bump does not rewrite an
    // earlier join, so mid stays non-zero and is not required to equal the
    // epoch of this call.
    CHECK(row.mid != 0, tag + ": IsolationDeny mid " + std::to_string(row.mid));
    CHECK(row.reason.find("isolation-deny:ref-tenant=") != std::string::npos,
          tag + ": IsolationDeny ref-tenant reason");
}

void expect_prints(Evaluator& ev, const Prim& fn, EvalValue arg, std::string_view needle,
                   std::string_view label) {
    const auto base = g_security_event_ring().seq.load(std::memory_order_acquire);
    EvalValue shown = make_void();
    const auto cap = capture_stdout([&] { shown = apply1(fn, arg); });
    const std::string tag(label);
    CHECK(cap.ok, tag + ": stdout capture succeeded");
    if (!cap.ok)
        return;
    CHECK(!is_error(shown), tag + ": not an error");
    CHECK(cap.out.find(needle) != std::string::npos, tag + ": payload printed");
    CHECK(isolation_rows_since(base, "display").count == 0, tag + ": no display IsolationDeny");
    CHECK(isolation_rows_since(base, "write").count == 0, tag + ": no write IsolationDeny");
    (void)ev;
}

} // namespace

int main() {
    std::println("=== Issue #4965 display/write list spine ===");

    arm_face("restricted", true);
    CHECK(aura::core::sandbox::is_sandbox_active() && !aura::core::sandbox::is_strict(),
          "4965: Restricted process mode");
    CHECK(aura::core::provenance::multi_tenant_env_active(), "4965: AURA_MULTI_TENANT armed");

    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.effect_sandbox_mode() == 1, "4965: Restricted+MT evaluator face is 1");

    const auto display = ev.primitives().lookup("display");
    const auto write = ev.primitives().lookup("write");
    const auto cons = ev.primitives().lookup("cons");
    const auto format = ev.primitives().lookup("format");
    const auto equal = ev.primitives().lookup("equal?");
    CHECK(display && write && cons && format && equal,
          "4965: display/write/cons/format/equal? present");
    if (!(display && write && cons && format && equal)) {
        std::println("\n=== #4965: {} passed, {} failed ===", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }

    EvalValue secret = make_void();
    {
        Evaluator::TenantScope a(ev, kTenantA, "4965-a");
        const auto idx = ev.push_string_heap(kSecret);
        secret = make_string(static_cast<std::uint64_t>(idx));
        CHECK(idx > 0 && std::string(kSecret).size() > 0, "4965: secret is a non-empty string");
        CHECK(ev.string_slot_tenant_record_for_test(static_cast<std::size_t>(idx)) == kTenantA + 1,
              "4965: secret stamp is tenant A + 1");
    }

    Evaluator::TenantScope b(ev, kTenantB, "4965-b");
    CHECK(ev.capability_tenant_id() == kTenantB, "4965: caller principal is B");

    for (int index : {63, 64, 65, 128}) {
        const auto lst = proper_list(cons, secret, index, index + 1);
        CHECK(is_pair(lst), "4965: B minted the list spine");
        const auto label = std::string("4965 Restricted+MT display index ") + std::to_string(index);
        const auto wlabel = std::string("4965 Restricted+MT write index ") + std::to_string(index);
        expect_deny_empty(ev, display, lst, "display", label);
        expect_deny_empty(ev, write, lst, "write", wlabel);
    }

    expect_deny_empty(ev, display, secret, "display", "4965 direct display of foreign string");
    expect_deny_empty(ev, write, secret, "write", "4965 direct write of foreign string");

    EvalValue own = make_void();
    {
        const auto idx = ev.push_string_heap(kOwn);
        own = make_string(static_cast<std::uint64_t>(idx));
        CHECK(ev.string_slot_tenant_record_for_test(static_cast<std::size_t>(idx)) == kTenantB + 1,
              "4965: owner string stamp is B + 1");
    }
    const auto own_long = proper_list(cons, own, 128, 129);
    expect_prints(ev, display, own_long, kOwn, "4965 owner display of a 129-element list");
    expect_prints(ev, write, own_long, kOwn, "4965 owner write of a 129-element list");

    // Longer than the old depth cap and larger than a pipe buffer, so a
    // capture that only reads after the call would deadlock on a pipe.
    constexpr int kWide = 20000;
    {
        const auto wide = proper_list(cons, secret, kWide - 1, kWide);
        expect_deny_empty(ev, display, wide, "display", "4965 Restricted+MT display index 19999");
        expect_deny_empty(ev, write, wide, "write", "4965 Restricted+MT write index 19999");
    }
    {
        const auto wide = proper_list(cons, own, kWide - 1, kWide);
        EvalValue shown = make_void();
        const auto cap = capture_stdout([&] { shown = apply1(display, wide); });
        CHECK(cap.ok, "4965 wide owner capture succeeded");
        if (cap.ok) {
            CHECK(!is_error(shown), "4965 wide owner display is not an error");
            CHECK(cap.out.size() > 65536, "4965 wide owner stdout exceeds a pipe buffer");
            CHECK(cap.out.find(kOwn) != std::string::npos, "4965 wide owner tail is printed");
            CHECK(cap.out.find(kSecret) == std::string::npos,
                  "4965 wide owner has no foreign secret");
        }
    }
    {
        // Time only display of an already built owner list. Floyd is linear;
        // a per-element scan of a growing vector would scale about 16x from
        // 2000 to 8000 cars.
        using clock = std::chrono::steady_clock;
        auto bench = [&](int n) -> long long {
            const auto lst = proper_list(cons, make_int(1), -1, n);
            if (!is_pair(lst))
                return -1;
            const auto warm = capture_stdout([&] { (void)apply1(display, lst); });
            if (!warm.ok)
                return -1;
            constexpr int kReps = 5;
            const auto t0 = clock::now();
            for (int i = 0; i < kReps; ++i) {
                const auto cap = capture_stdout([&] { (void)apply1(display, lst); });
                if (!cap.ok)
                    return -1;
            }
            return std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count();
        };
        const auto t2 = bench(2000);
        const auto t8 = bench(8000);
        std::println("4965 spine bench display us: n=2000 x5 -> {} ; n=8000 x5 -> {}", t2, t8);
        CHECK(t2 > 0 && t8 > 0, "4965 spine bench ran");
        if (t2 >= 1000)
            CHECK(t8 < t2 * 10, "4965 8000-car display stays under 10x the 2000-car time");
        else
            CHECK(t8 < 2'000'000, "4965 8000-car display finishes");
    }

    const auto nest64 = nest_singleton(cons, secret, 64);
    expect_deny_empty(ev, display, nest64, "display", "4965 nested depth 64 display");
    const auto nest65 = nest_singleton(cons, secret, 65);
    {
        EvalValue shown = make_void();
        const auto cap = capture_stdout([&] { shown = apply1(display, nest65); });
        CHECK(cap.ok, "4965 nested 65 capture succeeded");
        if (cap.ok) {
            CHECK(cap.out.find(kSecret) == std::string::npos,
                  "4965 nested 65 does not print the secret");
            CHECK(cap.out.find("...") != std::string::npos,
                  "4965 nested 65 truncates like the printer");
            CHECK(!is_error(shown), "4965 nested 65 is truncation, not a tenant deny");
        }
    }

    const auto dotted = cons_pair(cons, make_int(1), secret);
    expect_deny_empty(ev, display, dotted, "display", "4965 dotted foreign tail display");
    const auto dotted_own = cons_pair(cons, make_int(1), own);
    expect_prints(ev, display, dotted_own, kOwn, "4965 dotted owner tail display");

    {
        const auto other_idx = ev.push_string_heap("OTHER4965");
        const auto other = make_string(static_cast<std::uint64_t>(other_idx));
        const auto left = proper_list(cons, secret, 64, 65);
        const auto right = proper_list(cons, other, 64, 65);
        const auto base = g_security_event_ring().seq.load(std::memory_order_acquire);
        const auto eq = apply2(equal, left, right);
        const auto msg = error_text(ev, eq);
        CHECK(is_error(eq), "4965 equal? denies a foreign string at index 64");
        CHECK(msg.find("cross-tenant string slot read denied (#4505)") != std::string::npos,
              "4965 equal? keeps its existing reason");
        CHECK(isolation_rows_since(base, "equal?").count >= 1, "4965 equal? IsolationDeny");
        // at < 0 never selects `placed`; every car is the int i+1.
        const auto a = proper_list(cons, make_int(0), -1, 70);
        const auto b2 = proper_list(cons, make_int(0), -1, 70);
        const auto same = apply2(equal, a, b2);
        CHECK(is_bool(same) && as_bool(same), "4965 equal? still matches a 70-element owner list");

        const auto fmt_tmpl = make_string(static_cast<std::uint64_t>(ev.push_string_heap("~a")));
        const auto denied_fmt = apply2(format, fmt_tmpl, left);
        const auto fmt_msg = error_text(ev, denied_fmt);
        CHECK(is_error(denied_fmt), "4965 format denies a foreign string at index 64");
        CHECK(fmt_msg.find("format: cross-tenant string slot read denied (#4505)") !=
                  std::string::npos,
              "4965 format keeps its existing reason");
        const auto own_fmt = apply2(format, fmt_tmpl, own_long);
        CHECK(is_string(own_fmt), "4965 format prints an owner long list");
        if (is_string(own_fmt)) {
            const auto text =
                ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(own_fmt)));
            CHECK(text.find(kOwn) != std::string::npos,
                  "4965 format result contains the owner tail");
        }
    }

    // Owner cycle must terminate. A foreign car on a cycle is still denied
    // before any byte (the car is checked on the first visit).
    {
        const auto cell = cons_pair(cons, own, make_void());
        CHECK(is_pair(cell), "4965 owner cycle pair");
        if (is_pair(cell)) {
            const auto idx = as_pair_idx(cell);
            if (idx < ev.pairs().size())
                ev.pairs()[idx].cdr = cell;
            EvalValue shown = make_void();
            const auto cap = capture_stdout([&] { shown = apply1(display, cell); });
            CHECK(cap.ok, "4965 owner cycle capture succeeded");
            if (cap.ok) {
                CHECK(!is_error(shown), "4965 owner cycle display returns");
                CHECK(cap.out.find(kOwn) != std::string::npos,
                      "4965 owner cycle prints the car once");
                CHECK(cap.out.find("...") != std::string::npos, "4965 owner cycle stops");
            }
        }
        const auto foreign = cons_pair(cons, secret, make_void());
        if (is_pair(foreign)) {
            const auto idx = as_pair_idx(foreign);
            if (idx < ev.pairs().size())
                ev.pairs()[idx].cdr = foreign;
            expect_deny_empty(ev, display, foreign, "display", "4965 foreign cycle display");
        }
    }

    // Explicit Strict, multi-tenant off. Restricted single-tenant is not this face.
    arm_face("strict", false);
    CHECK(aura::core::sandbox::is_strict(), "4965: explicit Strict");
    CHECK(!aura::core::provenance::multi_tenant_env_active(), "4965: Strict does not require MT");
    CHECK(ev.effect_sandbox_mode() == 2, "4965: Strict evaluator face");
    {
        const auto lst = proper_list(cons, secret, 64, 65);
        expect_deny_empty(ev, display, lst, "display", "4965 Strict display index 64");
        expect_deny_empty(ev, write, lst, "write", "4965 Strict write index 64");
    }

    // Restricted single-tenant soft-share is the contract, not a hole.
    arm_face("restricted", false);
    CHECK(ev.effect_sandbox_mode() == 1, "4965: single-tenant Restricted face");
    CHECK(!aura::core::provenance::multi_tenant_env_active(), "4965: MT off");
    CHECK(!aura::core::sandbox::is_strict(), "4965: not Strict");
    {
        const auto lst = proper_list(cons, secret, 64, 65);
        expect_prints(ev, display, lst, kSecret, "4965 single-tenant Restricted still prints");
    }

    arm_face("off", false);
    CHECK(ev.effect_sandbox_mode() == 0, "4965: Soft/Off face");
    {
        const auto lst = proper_list(cons, secret, 64, 65);
        expect_prints(ev, display, lst, kSecret, "4965 Soft/Off still prints");
    }

    ::unsetenv("AURA_SANDBOX");
    ::unsetenv("AURA_MULTI_TENANT");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::core::provenance::set_multi_tenant_env_active(false);

    std::println("\n=== #4965: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
