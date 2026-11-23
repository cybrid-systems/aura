// evaluator_primitives_misc.cpp — P0 step 28: current-time / arena-offset
// primitives; Issue #2655: current-time-ms + monotonic-ms for sub-second
// denseness metrology.
// Issue #2915: proof migration onto register_prim + PrimSpec scaffold.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "prim_registrar_scaffold.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bus/transport.hh"
#include "bus/host.hh"
#include "bus/query.hh"
#include "bus/channel.hh"
#include "compiler/cct_probe.hh"
#include "compiler/mutate_type_gate.hh" // #5380: contract protection mirror
#include "compiler/evo_log_4910.hh"     // #5380: contract digest transport
#include "compiler/json_escape.hh"      // #5392: the evo schema export

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_error_idx;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;


namespace {

    struct BusSessionState {
        aura::bus::Attach att;
        // The workspace AST this session projects. Kept because a file-mode
        // eval clears current_flat between top-level forms.
        aura::ast::FlatAST* flat = nullptr;
        // Live-producer cursors (#5122): the last mutation id already
        // projected into each channel. Worldline slots are assigned in
        // mutation-log order so a patch can name the rebind that owns it.
        std::uint64_t patches_cursor = 0;
        std::uint64_t worldlines_cursor = 0;
        std::uint64_t next_wl_slot = 0;
        std::uint64_t patches_published = 0;
        std::uint64_t worldlines_published = 0;
        bool cct_published = false;
        // #5386: the static projection is a once-per-session snapshot too.
        std::uint64_t cct_static_count = 0;
        bool cct_static_live = false;
        std::vector<std::uint64_t> rebind_mids;
        std::unordered_map<std::uint64_t, std::uint64_t> wl_slot_by_mid;
        std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>>
            patch_range_by_slot;
    };

    std::mutex g_bus_mutex;
    std::unordered_map<std::int64_t, std::unique_ptr<BusSessionState>> g_bus_sessions;
    std::int64_t g_bus_next = 1;

    EvalValue bus_list(Evaluator& ev, std::initializer_list<EvalValue> values) {
        EvalValue out = make_void();
        auto it = values.end();
        while (it != values.begin()) {
            --it;
            out = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, out)));
        }
        return out;
    }

    EvalValue bus_string(Evaluator& ev, std::string_view text) {
        return make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::string(text))));
    }

    EvalValue bus_error(Evaluator& ev, std::string_view code, std::string_view message) {
        return bus_list(ev,
                        {bus_string(ev, "error"), bus_string(ev, code), bus_string(ev, message)});
    }

    // #5378: detect a make_merr failure pair (kind . (message . 0)) or a
    // raised error value. mutate:rebind / eval-current return these on
    // rejection; a plain #t / value is success.
    bool bus_is_failure(Evaluator& ev, const EvalValue& v) {
        if (is_error(v))
            return true;
        if (is_bool(v) && !as_bool(v))
            return true;
        if (!is_pair(v))
            return false;
        const auto oi = as_pair_idx(v);
        const auto pairs = ev.pairs();
        if (oi >= pairs.size())
            return false;
        const auto& outer = pairs[oi];
        if (!is_string(outer.car) || !is_pair(outer.cdr))
            return false;
        const auto ii = as_pair_idx(outer.cdr);
        if (ii >= pairs.size())
            return false;
        const auto& inner = pairs[ii];
        return is_string(inner.car) && is_int(inner.cdr) && as_int(inner.cdr) == 0;
    }

    std::string bus_failure_message(Evaluator& ev, const EvalValue& v) {
        const auto heap = ev.string_heap();
        const auto str = [&heap](const EvalValue& s) -> std::string {
            if (!is_string(s))
                return {};
            const auto idx = as_string_idx(s);
            return idx < heap.size() ? heap[idx] : std::string();
        };
        if (is_error(v))
            return "raised error " + std::to_string(as_error_idx(v));
        if (is_pair(v)) {
            const auto oi = as_pair_idx(v);
            const auto pairs = ev.pairs();
            if (oi < pairs.size() && is_string(pairs[oi].car)) {
                std::string msg = str(pairs[oi].car);
                if (is_pair(pairs[oi].cdr)) {
                    const auto ii = as_pair_idx(pairs[oi].cdr);
                    if (ii < pairs.size() && is_string(pairs[ii].car))
                        msg += ": " + str(pairs[ii].car);
                }
                return msg;
            }
        }
        return "rejected";
    }

    const char* bus_error_code(const aura::bus::TransportError& err) {
        switch (err.code) {
            case EACCES:
            case EPERM:
                return "permission";
            case EAGAIN:
                return "loan-exhausted";
            case ENOSPC:
                return "image-full";
            case ENOENT:
                // #5128: on attach, ENOENT is "no bus at this path" (the
                // socket does not exist) — a transport/discovery failure,
                // not an unknown session id. Session lookups report
                // unregistered-id directly, not through this mapping.
                return "transport-failed";
            case EINVAL:
                return "validation-failed";
            case EPROTO:
                return err.message.find("seal") != std::string::npos ? "seals-missing"
                                                                     : "schema-mismatch";
            default:
                return "transport-failed";
        }
    }

    bool bus_int(const EvalValue& v, std::int64_t* out) {
        if (!is_int(v))
            return false;
        *out = as_int(v);
        return true;
    }

    // Read one integer element of a built reply list (0 = status string).
    bool bus_reply_int(Evaluator& ev, EvalValue list, int index, std::int64_t* out) {
        EvalValue cur = list;
        for (int i = 0; i <= index; ++i) {
            if (!is_pair(cur))
                return false;
            const auto idx = as_pair_idx(cur);
            const auto pairs = ev.pairs();
            if (idx >= pairs.size())
                return false;
            if (i == index)
                return bus_int(pairs[idx].car, out);
            cur = pairs[idx].cdr;
        }
        return false;
    }

    // ── Live producers (#5122) ────────────────────────────────
    // A rebind is the identity a patch or worldline record belongs to.
    // The mutation log is append-only and its ids are increasing, so a
    // single forward scan assigns worldline slots deterministically.
    bool is_rebind_record(const aura::ast::MutationRecord& rec) {
        return rec.operator_name.find("rebind") != std::string::npos;
    }

    void note_rebinds_through(BusSessionState& state,
                              std::span<const aura::ast::MutationRecord> log,
                              std::uint64_t through_mid) {
        for (const auto& rec : log) {
            if (rec.mutation_id > through_mid)
                break;
            if (!is_rebind_record(rec))
                continue;
            if (state.wl_slot_by_mid.contains(rec.mutation_id))
                continue;
            state.wl_slot_by_mid.emplace(rec.mutation_id, state.next_wl_slot++);
            state.rebind_mids.push_back(rec.mutation_id);
        }
    }

    // The rebind in force at `mid` (the latest rebind at or before it).
    std::uint64_t worldline_slot_for(BusSessionState& state,
                                     std::span<const aura::ast::MutationRecord> log,
                                     std::uint64_t mid) {
        note_rebinds_through(state, log, mid);
        const auto it = std::upper_bound(state.rebind_mids.begin(), state.rebind_mids.end(), mid);
        if (it == state.rebind_mids.begin())
            return aura::bus::kImageNoIndex;
        return state.wl_slot_by_mid.at(*std::prev(it));
    }

    // Patch ranges are section-relative, the image directory is absolute.
    std::uint64_t section_relative(const aura::bus::Attach& att, aura::bus::SectionKind kind,
                                   std::uint64_t absolute) {
        aura::bus::SectionEntry e{};
        if (!aura::bus::find_section(att.view(), kind, &e))
            return absolute;
        return absolute >= e.offset ? absolute - e.offset : absolute;
    }

    aura::ast::FlatAST* active_workspace(Evaluator& ev, BusSessionState& state) {
        auto* flat = ev.workspace_flat();
        if (flat == nullptr)
            flat = ev.current_flat();
        if (flat != nullptr)
            state.flat = flat;
        return flat != nullptr ? flat : state.flat;
    }

    // ── #5378: KEEP → live workspace apply ─────────────────────
    // The coordinator thread calls this after the constraint + evidence
    // gates pass. It resolves each source-carrier patch to a define name,
    // snapshots the workspace, rebinds + re-evals, and restores the
    // snapshot on any failure (all-or-nothing). The eval thread and the
    // coordinator are serialized by the workspace mutation guards.
    struct BusApplyState {
        // Pre-canary snapshot id per worldline, for revert.
        std::unordered_map<std::uint64_t, std::int64_t> canary_snapshots;
    };
    std::unordered_map<std::uint64_t, BusApplyState> g_bus_apply;

    // #5397: the host's live define names and their node ids. A peer
    // that wants to rewrite a named function asks the host; the node
    // ids are host-local, so guessing them from another process is not
    // sound.
    bool bus_define_nodes(Evaluator& ev, std::vector<std::pair<std::string, std::uint64_t>>* out) {
        auto* flat = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (flat == nullptr || pool == nullptr)
            return false;
        out->clear();
        for (std::uint64_t id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            if (flat->tag(static_cast<aura::ast::NodeId>(id)) != aura::ast::NodeTag::Define)
                continue;
            const auto sym = flat->sym_id(static_cast<aura::ast::NodeId>(id));
            if (sym == aura::ast::INVALID_SYM)
                continue;
            out->emplace_back(std::string(pool->resolve(sym)), id);
        }
        return true;
    }

    // #5397: the mutable target allowlist. A bus KEEP may only rebind
    // the functions the host declares. The std/contract protection set is
    // a deny-list for the control plane; this is the allow-list for the
    // policy functions the closed loop is allowed to evolve. Empty (the
    // default) keeps the v0.3.1 surface: anything not protected may be
    // rebound.
    std::vector<std::string> bus_mutable_targets() {
        std::vector<std::string> out;
        const char* raw = ::getenv("AURA_BUS_MUTABLE_TARGETS");
        if (raw == nullptr)
            return out;
        const std::string list(raw);
        std::size_t at = 0;
        while (at <= list.size()) {
            const std::size_t comma = list.find(',', at);
            std::string item =
                list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
                item.erase(item.begin());
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
                item.pop_back();
            if (!item.empty())
                out.push_back(item);
            if (comma == std::string::npos)
                break;
            at = comma + 1;
        }
        return out;
    }

    // Resolve a workspace Define node id to its name (empty when absent).
    std::string bus_define_name(Evaluator& ev, std::uint64_t node) {
        auto* flat = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (flat == nullptr || pool == nullptr || node >= flat->size())
            return {};
        if (flat->is_free_slot(node) ||
            flat->tag(static_cast<aura::ast::NodeId>(node)) != aura::ast::NodeTag::Define)
            return {};
        const auto sym = flat->sym_id(static_cast<aura::ast::NodeId>(node));
        if (sym == aura::ast::INVALID_SYM)
            return {};
        return std::string(pool->resolve(sym));
    }

    // Call a registered primitive by name; returns false when absent.
    bool bus_call_prim(Evaluator& ev, std::string_view name, std::span<const EvalValue> args,
                       EvalValue* out) {
        auto fn = ev.primitives().lookup(name);
        if (!fn)
            return false;
        *out = (*fn)(args);
        return true;
    }

    bool bus_apply_keep(Evaluator& ev, std::uint64_t workspace, std::uint64_t worldline,
                        std::span<const aura::bus::PatchRecord> patches,
                        std::span<const std::string> sources, bool canary,
                        const std::vector<std::string>& mutable_targets, std::string* why) {
        const auto fail = [&](const std::string& msg) {
            if (why)
                *why = msg;
            return false;
        };
        auto* flat = ev.workspace_flat();
        if (flat == nullptr || ev.workspace_pool() == nullptr)
            return fail("no live workspace to apply into");
        // Collect source-carrier patches → (name, source). Immediate field
        // patches are image-only evidence, not live-AST applies.
        std::vector<std::pair<std::string, std::string>> rebinds;
        for (std::size_t i = 0; i < patches.size(); ++i) {
            const auto& p = patches[i];
            if (p.field_offset != aura::bus::kPatchSourceField)
                continue;
            const std::string name = bus_define_name(ev, p.node);
            if (name.empty())
                return fail("patch target is not a live workspace define");
            if (i >= sources.size() || sources[i].empty())
                return fail("source-carrier patch has no source text");
            rebinds.emplace_back(name, sources[i]);
        }
        if (rebinds.empty())
            return true; // nothing to apply to the live AST

        // #5397: the mutable target allowlist. A candidate may rewrite
        // several policy functions at once (choose-fn + admit-fn +
        // ttl-fn), but only the ones the host declared. The check runs
        // before the snapshot, so a refused candidate touches nothing.
        if (!mutable_targets.empty()) {
            for (const auto& [name, src] : rebinds) {
                if (std::find(mutable_targets.begin(), mutable_targets.end(), name) ==
                    mutable_targets.end()) {
                    return fail("patch target is not a mutable target: " + name);
                }
            }
        }

        // #5380: mirror the std/contract protection set before any rebind.
        // Refresh the installed digest, then check each target name.
        {
            const auto gen = aura::evo::contract_digest_gen();
            if (gen != aura::compiler::mutate_type_gate::installed_contract_gen())
                aura::compiler::mutate_type_gate::install_contract_digest(
                    gen == 0 ? std::string() : aura::evo::contract_digest_copy(), gen);
            for (const auto& [name, src] : rebinds) {
                const auto ruling = aura::compiler::mutate_type_gate::check_contract_mutation(
                    "mutate:rebind", name);
                if (ruling.rejected)
                    return fail(aura::compiler::mutate_type_gate::contract_refusal_message(
                        ruling, "mutate:rebind"));
            }
        }

        // Snapshot the workspace for all-or-nothing rollback.
        EvalValue snap = make_void();
        if (!bus_call_prim(ev, "mw:snapshot", {}, &snap) || !is_int(snap) || as_int(snap) < 0)
            return fail("workspace snapshot failed");
        const std::int64_t snap_id = as_int(snap);
        const auto restore = [&]() {
            EvalValue r = make_void();
            const EvalValue arg = make_int(snap_id);
            (void)bus_call_prim(ev, "mw:restore", {&arg, 1}, &r);
            // mw:restore re-binds the restored defines itself (#5149).
        };

        // Apply each rebind, then re-eval. Any failure restores the snapshot.
        for (const auto& [name, src] : rebinds) {
            const EvalValue name_v = bus_string(ev, name);
            const EvalValue src_v = bus_string(ev, src);
            const EvalValue args[2] = {name_v, src_v};
            EvalValue r = make_void();
            if (!bus_call_prim(ev, "mutate:rebind", args, &r)) {
                restore();
                return fail("mutate:rebind unavailable");
            }
            if (bus_is_failure(ev, r)) {
                restore();
                return fail("rebind rejected: " + bus_failure_message(ev, r));
            }
        }
        {
            EvalValue r = make_void();
            if (!bus_call_prim(ev, "eval-current", {}, &r)) {
                restore();
                return fail("eval-current unavailable");
            }
            if (bus_is_failure(ev, r)) {
                restore();
                return fail("eval rejected: " + bus_failure_message(ev, r));
            }
        }
        if (canary)
            g_bus_apply[workspace].canary_snapshots[worldline] = snap_id;
        return true;
    }

    bool bus_revert_keep(Evaluator& ev, std::uint64_t workspace, std::uint64_t worldline,
                         std::string* why) {
        auto it = g_bus_apply.find(workspace);
        if (it == g_bus_apply.end())
            return true;
        auto sit = it->second.canary_snapshots.find(worldline);
        if (sit == it->second.canary_snapshots.end())
            return true;
        const EvalValue arg = make_int(sit->second);
        EvalValue r = make_void();
        const bool ok = bus_call_prim(ev, "mw:restore", {&arg, 1}, &r) && !bus_is_failure(ev, r);
        // mw:restore restores the flat and re-binds the restored defines
        // (#5149), but the live value cells / IR closures may still hold
        // the canary body. Re-eval the workspace so the next call sees the
        // restored source (same shape as mw:try-mutation's rollback).
        if (ok) {
            EvalValue er = make_void();
            (void)bus_call_prim(ev, "eval-current", {}, &er);
        }
        it->second.canary_snapshots.erase(sit);
        if (!ok && why)
            *why = "canary revert restore failed";
        return ok;
    }

} // namespace


EvalValue bus_publish_record(Evaluator& ev, BusSessionState& state, int channel, const void* record,
                             std::size_t bytes, std::uint64_t* first_offset = nullptr,
                             bool* published = nullptr) {
    auto& att = state.att;
    aura::bus::LoanBody loan{};
    loan.channel = static_cast<std::uint64_t>(channel);
    loan.length = bytes;
    if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::Reserve, att.session,
                               {reinterpret_cast<const std::byte*>(&loan), sizeof(loan)}))
        return bus_error(ev, "transport-failed", "reserve send failed");
    aura::bus::FrameHeader hdr{};
    std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
    aura::bus::TransportError err;
    if (!aura::bus::recv_frame(att.sock, &hdr, body, &err))
        return bus_error(ev, "transport-failed", "reserve reply lost");
    if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
        aura::bus::ErrorBody eb{};
        std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
        return bus_error(ev, eb.code == ENOSPC ? "image-full" : "loan-exhausted", eb.message);
    }
    aura::bus::LoanBody granted{};
    std::memcpy(&granted, body.data(), std::min(sizeof(granted), body.size()));
    if (first_offset != nullptr)
        *first_offset = granted.offset;
    std::memcpy(static_cast<std::byte*>(att.base) + granted.offset, record, bytes);
    aura::bus::LoanBody pub = granted;
    pub.channel = static_cast<std::uint64_t>(channel);
    pub.length = bytes;
    if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::Publish, att.session,
                               {reinterpret_cast<const std::byte*>(&pub), sizeof(pub)}))
        return bus_error(ev, "transport-failed", "publish send failed");
    if (!aura::bus::recv_frame(att.sock, &hdr, body, &err))
        return bus_error(ev, "transport-failed", "publish reply lost");
    if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
        aura::bus::ErrorBody eb{};
        std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
        return bus_error(ev, "validation-failed", eb.message);
    }
    aura::bus::ChannelBody cb{};
    std::memcpy(&cb, body.data(), std::min(sizeof(cb), body.size()));
    if (published != nullptr)
        *published = true;
    return bus_list(ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(cb.generation)),
                         make_int(static_cast<std::int64_t>(cb.count))});
}

void register_misc_primitives(PrimRegistrar add, Evaluator& ev) {
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;

    // Wall-clock seconds since Unix epoch (legacy; coarse for denseness).
    register_prim(
        add, ev, "current-time",
        [](const auto&) -> EvalValue {
            return make_int(static_cast<std::int64_t>(std::time(nullptr)));
        },
        pure_general(0, "() -> int", "Wall-clock seconds since Unix epoch."));

    // Issue #2655: wall-clock milliseconds since Unix epoch (system_clock).
    // Suitable for timestamps; for elapsed prefer monotonic-ms (steady_clock
    // is not subject to wall-clock steps).
    register_prim(
        add, ev, "current-time-ms",
        [](const auto&) -> EvalValue {
            using clock = std::chrono::system_clock;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                clock::now().time_since_epoch())
                                .count();
            return make_int(static_cast<std::int64_t>(ms));
        },
        pure_general(0, "() -> int", "Wall-clock milliseconds since Unix epoch (system_clock)."));

    // Issue #2655: steady_clock milliseconds (process-relative origin).
    // Preferred for denseness elapsed / ops-s metrology on short pure loops.
    register_prim(
        add, ev, "monotonic-ms",
        [](const auto&) -> EvalValue {
            using clock = std::chrono::steady_clock;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                clock::now().time_since_epoch())
                                .count();
            return make_int(static_cast<std::int64_t>(ms));
        },
        pure_general(0, "() -> int",
                     "Steady-clock milliseconds (process-relative; denseness elapsed)."));


    register_prim(
        add, ev, "bus-call",
        [&ev](const std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_string(a[0]))
                return bus_error(ev, "validation-failed", "bus-call needs a string op");
            const std::string op = ev.string_at_or(as_string_idx(a[0]));

            std::lock_guard lock(g_bus_mutex);
            const auto session = [&](std::size_t index) -> BusSessionState* {
                std::int64_t sid = 0;
                if (index >= a.size() || !bus_int(a[index], &sid))
                    return nullptr;
                const auto it = g_bus_sessions.find(sid);
                return it == g_bus_sessions.end() ? nullptr : it->second.get();
            };
            const auto send_loan = [&](int fd, std::uint64_t session_id, aura::bus::FrameType type,
                                       const aura::bus::LoanBody& loan) {
                return aura::bus::send_frame(
                    fd, type, session_id,
                    {reinterpret_cast<const std::byte*>(&loan), sizeof(loan)});
            };
            const auto recv_reply = [&](int fd, aura::bus::FrameHeader* header,
                                        std::vector<std::byte>* body) {
                aura::bus::TransportError err;
                return aura::bus::recv_frame(fd, header, *body, &err);
            };

            if (op == "attach") {
                if (a.size() < 6 || !is_string(a[1]))
                    return bus_error(ev, "validation-failed",
                                     "attach needs path, ws, role, caps, channels");
                std::int64_t ws = 0, role = 0, caps = 0, channels = 0;
                if (!bus_int(a[2], &ws) || !bus_int(a[3], &role) || !bus_int(a[4], &caps) ||
                    !bus_int(a[5], &channels))
                    return bus_error(ev, "validation-failed", "attach arguments must be integers");
                std::string path = ev.string_at_or(as_string_idx(a[1]));
                if (path.empty())
                    path = aura::bus::default_socket_path(static_cast<std::uint64_t>(ws));
                auto state = std::make_unique<BusSessionState>();
                aura::bus::Client client;
                aura::bus::TransportError err;
                if (!client.connect_to(path, static_cast<std::uint64_t>(ws),
                                       static_cast<std::uint32_t>(role),
                                       static_cast<std::uint64_t>(caps), &state->att, &err))
                    return bus_error(ev, bus_error_code(err), err.message);
                const std::int64_t sid = g_bus_next++;
                const std::uint64_t size = state->att.size;
                const std::uint64_t granted = state->att.caps;
                g_bus_sessions[sid] = std::move(state);
                // #5408: size and the granted capability mask are raw
                // 64-bit values from HelloOk; promote instead of aborting
                // when one is outside the fixnum domain.
                return bus_list(ev,
                                {bus_string(ev, "ok"), make_int(sid),
                                 types::make_int_or_promote(static_cast<std::int64_t>(size)),
                                 types::make_int_or_promote(static_cast<std::int64_t>(granted))});
            }

            if (op == "detach") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "detach needs a session id");
                const auto it = g_bus_sessions.find(sid);
                if (it == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                // Send a Disconnect frame so the coordinator ends the
                // session cleanly (rolls back pending trials, reclaims
                // loans) before the socket closes.
                aura::bus::LoanBody bye{};
                (void)aura::bus::send_frame(
                    it->second->att.sock, aura::bus::FrameType::Disconnect, it->second->att.session,
                    {reinterpret_cast<const std::byte*>(&bye), sizeof(bye)});
                g_bus_sessions.erase(it);
                return bus_list(ev, {bus_string(ev, "ok")});
            }

            // Issue #5128: the workspace owner hosts the bus (#5121's
            // process model). host is idempotent per workspace; the
            // serving thread is the delivered single-peer loop.
            if (op == "host") {
                std::int64_t ws = 0, bytes = 0;
                if (a.size() < 2 || !bus_int(a[1], &ws))
                    return bus_error(ev, "validation-failed", "host needs a workspace id");
                if (a.size() >= 3 && !bus_int(a[2], &bytes))
                    return bus_error(ev, "validation-failed",
                                     "host image bytes must be an integer");
                std::string path;
                if (a.size() >= 4) {
                    if (!is_string(a[3]))
                        return bus_error(ev, "validation-failed", "host path must be a string");
                    path = ev.string_at_or(as_string_idx(a[3]));
                }
                aura::bus::HostOptions opts;
                opts.workspace = static_cast<std::uint64_t>(ws);
                opts.image_bytes = bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
                opts.path = path;
                if (const char* sb = ::getenv("AURA_SANDBOX")) {
                    const std::string preset = sb;
                    if (preset == "ci")
                        opts.preset = aura::bus::SandboxPreset::Ci;
                    else if (preset == "prod")
                        opts.preset = aura::bus::SandboxPreset::Prod;
                }
                // #5378/#5381: wire the live-workspace apply/revert. The
                // coordinator thread runs these; the mutation guards
                // serialize them against the eval thread. Evidence-gated
                // KEEP and canary observation are opt-in via env so the
                // v0.3.0 surface stays the default.
                opts.workspace_generation = ev.workspace_flat_generation();
                // #5397: the mutable target allowlist (empty = unrestricted).
                const std::vector<std::string> mutable_targets = bus_mutable_targets();
                opts.apply = [&ev, ws,
                              mutable_targets](std::uint64_t wl, std::uint64_t gen,
                                               std::span<const aura::bus::PatchRecord> patches,
                                               std::span<const std::string> sources, bool canary,
                                               std::string* why) {
                    (void)gen;
                    return bus_apply_keep(ev, static_cast<std::uint64_t>(ws), wl, patches, sources,
                                          canary, mutable_targets, why);
                };
                opts.revert = [&ev, ws](std::uint64_t wl, std::string* why) {
                    return bus_revert_keep(ev, static_cast<std::uint64_t>(ws), wl, why);
                };
                // #5397: the host's define names, so a peer can address
                // a named function.
                opts.define_names = [&ev](std::vector<std::pair<std::string, std::uint64_t>>* out) {
                    return bus_define_nodes(ev, out);
                };
                if (const char* ev_req = ::getenv("AURA_BUS_EVIDENCE"))
                    opts.evidence_required = std::string(ev_req) == "1";
                if (const char* cw = ::getenv("AURA_BUS_CANARY_WINDOW"))
                    opts.canary_window = std::strtoull(cw, nullptr, 10);
                if (const char* cf = ::getenv("AURA_BUS_CANARY_FLOOR"))
                    opts.canary_floor = std::strtoull(cf, nullptr, 10);
                if (const char* ck = ::getenv("AURA_BUS_CANARY_KEY"))
                    opts.canary_metric_key = std::strtoull(ck, nullptr, 10);
                // #5391: the statistical admission policy. Off by
                // default; a host opts in and sets the bar, the window
                // floor, the negative-KEEP budget and its window.
                if (const char* gate_on = ::getenv("AURA_BUS_GATE"))
                    opts.gate.required = std::string(gate_on) == "1";
                if (const char* gt = ::getenv("AURA_BUS_GATE_THRESHOLD"))
                    opts.gate.threshold_ppm = std::strtoull(gt, nullptr, 10);
                if (const char* gw = ::getenv("AURA_BUS_GATE_MIN_WINDOWS"))
                    opts.gate.min_windows =
                        static_cast<std::uint32_t>(std::strtoull(gw, nullptr, 10));
                if (const char* gb = ::getenv("AURA_BUS_GATE_BUDGET"))
                    opts.gate.negative_budget_ppm = std::strtoull(gb, nullptr, 10);
                if (const char* gs = ::getenv("AURA_BUS_GATE_WINDOW"))
                    opts.gate.budget_window =
                        static_cast<std::uint32_t>(std::strtoull(gs, nullptr, 10));
                if (const char* gc = ::getenv("AURA_BUS_GATE_CONTROL"))
                    opts.gate.require_control = std::string(gc) != "0";
                // #5396: the adversarial peer quotas — the frame
                // rate, the window, the handshake deadline and the
                // constraint-change quota.
                if (const char* pf = ::getenv("AURA_BUS_PEER_FRAMES"))
                    opts.peer_quota.frame_rate = std::strtoull(pf, nullptr, 10);
                if (const char* pb = ::getenv("AURA_BUS_PEER_BURST"))
                    opts.peer_quota.frame_burst = std::strtoull(pb, nullptr, 10);
                if (const char* ph = ::getenv("AURA_BUS_PEER_HELLO_MS"))
                    opts.peer_quota.hello_timeout_ms = std::strtoull(ph, nullptr, 10);
                if (const char* pc = ::getenv("AURA_BUS_PEER_CONSTRAINT_CHANGES"))
                    opts.peer_quota.constraint_changes_per_window = std::strtoull(pc, nullptr, 10);
                if (const char* cw = ::getenv("AURA_BUS_PEER_CONSTRAINT_WINDOW_MS"))
                    opts.peer_quota.constraint_window_ms = std::strtoull(cw, nullptr, 10);
                // #5403: task/global token, wall-clock and failure budgets.
                if (const char* v = ::getenv("AURA_BUS_TASK_TOKEN_LIMIT"))
                    opts.budget.task.token_limit = std::strtoull(v, nullptr, 10);
                if (const char* v = ::getenv("AURA_BUS_TASK_WALL_MS"))
                    opts.budget.task.wall_limit_ms = std::strtoull(v, nullptr, 10);
                if (const char* v = ::getenv("AURA_BUS_TASK_FAILURE_LIMIT"))
                    opts.budget.task.failure_limit = std::strtoull(v, nullptr, 10);
                if (const char* v = ::getenv("AURA_BUS_GLOBAL_TOKEN_LIMIT"))
                    opts.budget.global.token_limit = std::strtoull(v, nullptr, 10);
                if (const char* v = ::getenv("AURA_BUS_GLOBAL_WALL_MS"))
                    opts.budget.global.wall_limit_ms = std::strtoull(v, nullptr, 10);
                if (const char* v = ::getenv("AURA_BUS_GLOBAL_FAILURE_LIMIT"))
                    opts.budget.global.failure_limit = std::strtoull(v, nullptr, 10);
                // #5393: the round budget — the wall clock a round may
                // take, the cooldown between two rounds and the round
                // cap.
                if (const char* rw = ::getenv("AURA_BUS_ROUND_WALL_MS"))
                    opts.round_budget.wall_ms = std::strtoull(rw, nullptr, 10);
                if (const char* rc = ::getenv("AURA_BUS_ROUND_COOLDOWN_MS"))
                    opts.round_budget.cooldown_ms = std::strtoull(rc, nullptr, 10);
                if (const char* rm = ::getenv("AURA_BUS_ROUND_MAX"))
                    opts.round_budget.max_rounds = std::strtoull(rm, nullptr, 10);
                // #5394: the degradation detector. The watched metric
                // keys come first; the keys where a *rise* is the
                // degradation follow.
                if (const char* keys = ::getenv("AURA_BUS_DEGRADE_KEYS")) {
                    const std::string list(keys);
                    std::size_t at = 0;
                    while (at < list.size()) {
                        const std::size_t comma = list.find(',', at);
                        const std::string item = list.substr(
                            at, comma == std::string::npos ? std::string::npos : comma - at);
                        if (!item.empty()) {
                            aura::bus::DegradeKey watched{};
                            watched.key = std::strtoull(item.c_str(), nullptr, 10);
                            watched.higher_is_worse = false;
                            opts.degrade.keys.push_back(watched);
                        }
                        if (comma == std::string::npos)
                            break;
                        at = comma + 1;
                    }
                }
                if (const char* up = ::getenv("AURA_BUS_DEGRADE_UP")) {
                    const std::string list(up);
                    std::size_t at = 0;
                    while (at < list.size()) {
                        const std::size_t comma = list.find(',', at);
                        const std::string item = list.substr(
                            at, comma == std::string::npos ? std::string::npos : comma - at);
                        if (!item.empty()) {
                            const std::uint64_t key = std::strtoull(item.c_str(), nullptr, 10);
                            for (aura::bus::DegradeKey& watched : opts.degrade.keys) {
                                if (watched.key == key)
                                    watched.higher_is_worse = true;
                            }
                        }
                        if (comma == std::string::npos)
                            break;
                        at = comma + 1;
                    }
                }
                if (const char* ms = ::getenv("AURA_BUS_DEGRADE_MIN_SAMPLES"))
                    opts.degrade.min_samples = std::strtoull(ms, nullptr, 10);
                if (const char* al = ::getenv("AURA_BUS_DEGRADE_ALPHA"))
                    opts.degrade.ewma_alpha_ppm = std::strtoull(al, nullptr, 10);
                if (const char* dr = ::getenv("AURA_BUS_DEGRADE_DRIFT"))
                    opts.degrade.drift_ppm = std::strtoull(dr, nullptr, 10);
                if (const char* li = ::getenv("AURA_BUS_DEGRADE_LIMIT"))
                    opts.degrade.limit_ppm = std::strtoull(li, nullptr, 10);
                if (const char* ca = ::getenv("AURA_BUS_DEGRADE_CALM"))
                    opts.degrade.calm_samples = std::strtoull(ca, nullptr, 10);
                std::string out_path, why;
                if (!aura::bus::host_start(opts, &out_path, &why))
                    return bus_error(ev, "transport-failed", why);
                return bus_list(ev, {bus_string(ev, "ok"), bus_string(ev, out_path), make_int(ws)});
            }

            if (op == "unhost") {
                std::int64_t ws = 0;
                if (a.size() < 2 || !bus_int(a[1], &ws))
                    return bus_error(ev, "validation-failed", "unhost needs a workspace id");
                std::string why;
                if (!aura::bus::host_stop(static_cast<std::uint64_t>(ws), &why))
                    return bus_error(ev, "unregistered-id", why);
                return bus_list(ev, {bus_string(ev, "ok")});
            }

            if (op == "host?") {
                std::int64_t ws = 0;
                if (a.size() < 2 || !bus_int(a[1], &ws))
                    return bus_error(ev, "validation-failed", "host? needs a workspace id");
                return bus_list(
                    ev, {bus_string(ev, "ok"),
                         make_int(aura::bus::host_active(static_cast<std::uint64_t>(ws)) ? 1 : 0)});
            }

            BusSessionState* state = session(1);
            if (state == nullptr)
                return bus_error(ev, "unregistered-id", "unknown session");
            auto& att = state->att;

            // #5399: one task-board verb. The reply is a TaskReplyBody;
            // a task refusal maps its errno-style code to the bus error
            // vocabulary and keeps the precise reason in the message.
            const auto task_call = [&](std::uint64_t task_op, std::uint64_t task_id,
                                       std::uint64_t parent, std::uint64_t lease_ms,
                                       std::uint64_t ref, std::uint64_t flags) -> EvalValue {
                aura::bus::TaskRequestBody req{};
                req.op = task_op;
                req.task_id = task_id;
                req.parent = parent;
                req.lease_ms = lease_ms;
                req.ref = ref;
                req.flags = flags;
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::TaskAction, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "task send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "task reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    std::string code = "validation-failed";
                    if (eb.code == EPERM)
                        code = "permission";
                    else if (eb.code == ENOENT)
                        code = "unregistered-id";
                    return bus_error(ev, code, eb.message);
                }
                if (hdr.type != static_cast<std::uint16_t>(aura::bus::FrameType::TaskReply) ||
                    hdr.len < sizeof(aura::bus::TaskReplyBody))
                    return bus_error(ev, "validation-failed", "malformed task reply");
                aura::bus::TaskReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                return bus_list(ev, {
                                        bus_string(ev, "ok"),
                                        make_int(static_cast<std::int64_t>(reply.task_id)),
                                        make_int(static_cast<std::int64_t>(reply.state)),
                                        make_int(static_cast<std::int64_t>(reply.owner)),
                                        make_int(static_cast<std::int64_t>(reply.lease_epoch)),
                                        make_int(static_cast<std::int64_t>(reply.deadline_ms)),
                                        make_int(static_cast<std::int64_t>(reply.parent)),
                                        make_int(static_cast<std::int64_t>(reply.ref)),
                                        make_int(static_cast<std::int64_t>(reply.count)),
                                    });
            };

            // #5402: contract create/status/evidence.
            const auto contract_call = [&](std::uint64_t contract_op, std::uint64_t task,
                                           std::uint64_t test_ref, std::uint64_t metric_floor,
                                           std::uint64_t tests_passed,
                                           std::uint64_t metric) -> EvalValue {
                aura::bus::ContractRequestBody req{};
                req.op = contract_op;
                req.task = task;
                req.test_ref = test_ref;
                req.metric_floor = metric_floor;
                req.tests_passed = tests_passed;
                req.metric = metric;
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::ContractAction,
                                           att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "contract send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "contract reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, eb.code == EPERM ? "permission" : "validation-failed",
                                     eb.message);
                }
                if (hdr.type != static_cast<std::uint16_t>(aura::bus::FrameType::ContractReply) ||
                    hdr.len < sizeof(aura::bus::ContractReplyBody))
                    return bus_error(ev, "validation-failed", "malformed contract reply");
                aura::bus::ContractReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                // #5400: the contract fingerprint is a full 64-bit FNV
                // hash; a fixnum is 62-bit. Keep the low 62 bits in the
                // list surface (the board keeps the full value).
                return bus_list(
                    ev,
                    {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(reply.task)),
                     make_int(static_cast<std::int64_t>(reply.fingerprint & 0x3fffffffffffffffull)),
                     make_int(static_cast<std::int64_t>(reply.state)),
                     make_int(static_cast<std::int64_t>(reply.test_ref)),
                     make_int(static_cast<std::int64_t>(reply.metric_floor)),
                     make_int(static_cast<std::int64_t>(reply.tests_passed)),
                     make_int(static_cast<std::int64_t>(reply.metric)),
                     make_int(static_cast<std::int64_t>(reply.has_evidence))});
            };

            // #5403: task/global budget report/charge.
            const auto budget_call = [&](std::uint64_t budget_op, std::uint64_t task,
                                         std::uint64_t tokens, std::uint64_t wall_ms,
                                         std::uint64_t failures) -> EvalValue {
                aura::bus::BudgetRequestBody req{};
                req.op = budget_op;
                req.task = task;
                req.tokens = tokens;
                req.wall_ms = wall_ms;
                req.failures = failures;
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::BudgetAction,
                                           att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "budget send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "budget reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, eb.code == EPERM ? "permission" : "validation-failed",
                                     eb.message);
                }
                if (hdr.type != static_cast<std::uint16_t>(aura::bus::FrameType::BudgetReply) ||
                    hdr.len < sizeof(aura::bus::BudgetReplyBody))
                    return bus_error(ev, "validation-failed", "malformed budget reply");
                aura::bus::BudgetReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.task)),
                                     make_int(static_cast<std::int64_t>(reply.tokens)),
                                     make_int(static_cast<std::int64_t>(reply.token_limit)),
                                     make_int(static_cast<std::int64_t>(reply.wall_ms)),
                                     make_int(static_cast<std::int64_t>(reply.wall_limit_ms)),
                                     make_int(static_cast<std::int64_t>(reply.failures)),
                                     make_int(static_cast<std::int64_t>(reply.failure_limit)),
                                     make_int(static_cast<std::int64_t>(reply.flags))});
            };

            if (op == "subscribe") {
                std::int64_t channel = 0;
                if (a.size() < 3 || !bus_int(a[2], &channel))
                    return bus_error(ev, "validation-failed", "subscribe needs a channel");
                aura::bus::LoanBody loan{};
                loan.channel = static_cast<std::uint64_t>(channel);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::Subscribe, loan))
                    return bus_error(ev, "transport-failed", "subscribe send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "subscribe reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::ChannelBody))
                    return bus_error(ev, "validation-failed", "malformed subscribe reply");
                aura::bus::ChannelBody cb{};
                std::memcpy(&cb, body.data(), sizeof(cb));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(cb.generation)),
                                     make_int(static_cast<std::int64_t>(cb.count)),
                                     make_int(static_cast<std::int64_t>(cb.offset)),
                                     make_int(static_cast<std::int64_t>(cb.length)),
                                     make_int(static_cast<std::int64_t>(cb.snapshot))});
            }

            if (op == "query") {
                std::int64_t channel = 0, since_gen = 0, since_count = 0;
                if (a.size() < 5 || !bus_int(a[2], &channel) || !bus_int(a[3], &since_gen) ||
                    !bus_int(a[4], &since_count))
                    return bus_error(ev, "validation-failed",
                                     "query needs channel, since generation and count");
                aura::bus::CatchUpBody req{};
                req.channel = static_cast<std::uint64_t>(channel);
                req.generation = static_cast<std::uint64_t>(since_gen);
                req.count = static_cast<std::uint64_t>(since_count);
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::CatchUp, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "query send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "query reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::ChannelBody))
                    return bus_error(ev, "validation-failed", "malformed query reply");
                aura::bus::ChannelBody cb{};
                std::memcpy(&cb, body.data(), sizeof(cb));
                const auto view = att.view();
                if (cb.offset > view.size() || cb.length > view.size() - cb.offset)
                    return bus_error(ev, "validation-failed", "query range outside the image");
                std::vector<EvalValue> rows;
                // #5385: cb.count is the reply range's record count; the
                // element size comes from the range, not from the total.
                const std::size_t elem =
                    cb.count == 0 ? 0 : static_cast<std::size_t>(cb.length / cb.count);
                const std::byte* base = view.data() + cb.offset;
                for (std::uint64_t i = 0; i < cb.count; ++i) {
                    const std::byte* rec = base + i * elem;
                    if (channel == 1 && elem >= sizeof(aura::bus::MetricRecord)) {
                        aura::bus::MetricRecord m{};
                        std::memcpy(&m, rec, sizeof(m));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(m.key_sym)),
                                          make_int(static_cast<std::int64_t>(m.value))}));
                    } else if (channel == 2 && elem >= sizeof(aura::bus::CctRecord)) {
                        aura::bus::CctRecord c{};
                        std::memcpy(&c, rec, sizeof(c));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(c.parent)),
                                          make_int(static_cast<std::int64_t>(c.mid_lo)),
                                          make_int(static_cast<std::int64_t>(c.node)),
                                          make_int(static_cast<std::int64_t>(c.samples)),
                                          make_int(static_cast<std::int64_t>(c.self_ns))}));
                    } else if (channel == 3 && elem >= sizeof(aura::bus::PatchRecord)) {
                        aura::bus::PatchRecord p{};
                        std::memcpy(&p, rec, sizeof(p));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(p.node)),
                                          make_int(static_cast<std::int64_t>(p.field_offset)),
                                          make_int(static_cast<std::int64_t>(p.new_value)),
                                          make_int(static_cast<std::int64_t>(p.worldline)),
                                          make_int(static_cast<std::int64_t>(p.generation))}));
                    } else if (channel == 4 && elem >= aura::bus::kWorldlineV1Size) {
                        aura::bus::WorldlineRecord w{};
                        std::memcpy(&w, rec, std::min<std::size_t>(elem, sizeof(w)));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(w.id)),
                                          make_int(static_cast<std::int64_t>(w.parent)),
                                          make_int(w.state), make_int(w.constraint_result),
                                          make_int(static_cast<std::int64_t>(w.patch_off)),
                                          make_int(static_cast<std::int64_t>(w.patch_len))}));
                    } else if (channel == 5 && elem >= sizeof(aura::bus::NodeRecord)) {
                        aura::bus::NodeRecord n{};
                        std::memcpy(&n, rec, sizeof(n));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(n.kind)),
                                          make_int(static_cast<std::int64_t>(n.slot)),
                                          make_int(static_cast<std::int64_t>(n.sym)),
                                          make_int(static_cast<std::int64_t>(n.parent)),
                                          make_int(static_cast<std::int64_t>(n.first_child))}));
                    } else if (channel == 6 && elem >= sizeof(aura::bus::TaskRecord)) {
                        aura::bus::TaskRecord t{};
                        std::memcpy(&t, rec, sizeof(t));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(t.task_id)),
                                          make_int(static_cast<std::int64_t>(t.parent)),
                                          make_int(static_cast<std::int64_t>(t.kind)),
                                          make_int(static_cast<std::int64_t>(t.actor)),
                                          make_int(static_cast<std::int64_t>(t.lease_epoch)),
                                          make_int(static_cast<std::int64_t>(t.deadline_ms)),
                                          make_int(static_cast<std::int64_t>(t.ref))}));
                    }
                }
                EvalValue listed = make_void();
                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                    listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(cb.generation)),
                                     make_int(static_cast<std::int64_t>(cb.total)),
                                     make_int(static_cast<std::int64_t>(cb.snapshot)), listed});
            }

            // #5399/#5404: the task board. post-task takes an optional
            // flags word (bit 0 = needs approval).
            if (op == "post-task" || op == "claim" || op == "heartbeat" || op == "complete" ||
                op == "fail" || op == "task-status") {
                std::int64_t arg1 = 0, arg2 = 0, flags = 0;
                if (op == "post-task") {
                    if (a.size() < 4 || !bus_int(a[2], &arg1) || !bus_int(a[3], &arg2))
                        return bus_error(ev, "validation-failed", "post-task needs parent and ref");
                    if (a.size() >= 5 && !bus_int(a[4], &flags))
                        return bus_error(ev, "validation-failed",
                                         "post-task flags must be an integer");
                    return task_call(0, 0, static_cast<std::uint64_t>(arg1), 0,
                                     static_cast<std::uint64_t>(arg2),
                                     static_cast<std::uint64_t>(flags));
                }
                if (op == "claim") {
                    if (a.size() < 3 || !bus_int(a[2], &arg1))
                        return bus_error(ev, "validation-failed", "claim needs a lease");
                    if (a.size() >= 4 && !bus_int(a[3], &arg2))
                        return bus_error(ev, "validation-failed",
                                         "claim task id is not an integer");
                    return task_call(1, static_cast<std::uint64_t>(arg2), 0,
                                     static_cast<std::uint64_t>(arg1), 0, 0);
                }
                if (op == "task-status") {
                    if (a.size() < 3 || !bus_int(a[2], &arg1))
                        return bus_error(ev, "validation-failed", "task-status needs an id");
                    return task_call(5, static_cast<std::uint64_t>(arg1), 0, 0, 0, 0);
                }
                if (a.size() < 4 || !bus_int(a[2], &arg1) || !bus_int(a[3], &arg2))
                    return bus_error(ev, "validation-failed", "task verb needs id and value");
                if (op == "heartbeat")
                    return task_call(2, static_cast<std::uint64_t>(arg1), 0,
                                     static_cast<std::uint64_t>(arg2), 0, 0);
                if (op == "complete")
                    return task_call(3, static_cast<std::uint64_t>(arg1), 0, 0,
                                     static_cast<std::uint64_t>(arg2), 0);
                if (op == "fail")
                    return task_call(4, static_cast<std::uint64_t>(arg1), 0, 0,
                                     static_cast<std::uint64_t>(arg2), 0);
                return task_call(5, static_cast<std::uint64_t>(arg1), 0, 0, 0, 0);
            }

            // #5404: request/approve/deny an approval object.
            if (op == "approve") {
                std::int64_t object = 0, decision = 0, timeout = 0;
                if (a.size() < 4 || !bus_int(a[2], &object) || !bus_int(a[3], &decision))
                    return bus_error(ev, "validation-failed", "approve needs object and decision");
                if (a.size() >= 5 && !bus_int(a[4], &timeout))
                    return bus_error(ev, "validation-failed", "approve timeout must be an integer");
                aura::bus::ApprovalRequestBody req{};
                req.object = static_cast<std::uint64_t>(object);
                req.decision = static_cast<std::uint64_t>(decision);
                req.timeout_ms = static_cast<std::uint64_t>(timeout);
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::ApprovalAction,
                                           att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "approve send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "approve reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, eb.code == EPERM ? "permission" : "validation-failed",
                                     eb.message);
                }
                if (hdr.type != static_cast<std::uint16_t>(aura::bus::FrameType::ApprovalReply) ||
                    hdr.len < sizeof(aura::bus::ApprovalReplyBody))
                    return bus_error(ev, "validation-failed", "malformed approve reply");
                aura::bus::ApprovalReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.object)),
                                     make_int(static_cast<std::int64_t>(reply.state)),
                                     make_int(static_cast<std::int64_t>(reply.deadline_ms)),
                                     make_int(static_cast<std::int64_t>(reply.actor))});
            }

            // #5402: executable acceptance contracts.
            if (op == "contract") {
                std::int64_t task = 0, test_ref = 0, floor = 0;
                if (a.size() < 5 || !bus_int(a[2], &task) || !bus_int(a[3], &test_ref) ||
                    !bus_int(a[4], &floor))
                    return bus_error(ev, "validation-failed",
                                     "contract needs task, test ref and metric floor");
                return contract_call(0, static_cast<std::uint64_t>(task),
                                     static_cast<std::uint64_t>(test_ref),
                                     static_cast<std::uint64_t>(floor), 0, 0);
            }
            if (op == "contract-status") {
                std::int64_t task = 0;
                if (a.size() < 3 || !bus_int(a[2], &task))
                    return bus_error(ev, "validation-failed", "contract-status needs a task");
                return contract_call(1, static_cast<std::uint64_t>(task), 0, 0, 0, 0);
            }
            if (op == "contract-evidence") {
                std::int64_t task = 0, tests = 0, metric = 0;
                if (a.size() < 5 || !bus_int(a[2], &task) || !bus_int(a[3], &tests) ||
                    !bus_int(a[4], &metric))
                    return bus_error(ev, "validation-failed",
                                     "contract-evidence needs task, tests and metric");
                return contract_call(2, static_cast<std::uint64_t>(task), 0, 0,
                                     static_cast<std::uint64_t>(tests),
                                     static_cast<std::uint64_t>(metric));
            }

            // #5403: task/global budgets.
            if (op == "budget-report") {
                std::int64_t task = 0;
                if (a.size() >= 3 && !bus_int(a[2], &task))
                    return bus_error(ev, "validation-failed",
                                     "budget-report task must be an integer");
                return budget_call(0, static_cast<std::uint64_t>(task), 0, 0, 0);
            }
            if (op == "budget-charge") {
                std::int64_t task = 0, tokens = 0, wall = 0, failures = 0;
                if (a.size() < 4 || !bus_int(a[2], &task) || !bus_int(a[3], &tokens))
                    return bus_error(ev, "validation-failed",
                                     "budget-charge needs task and tokens");
                if (a.size() >= 5 && !bus_int(a[4], &wall))
                    return bus_error(ev, "validation-failed", "budget wall must be an integer");
                if (a.size() >= 6 && !bus_int(a[5], &failures))
                    return bus_error(ev, "validation-failed", "budget failures must be an integer");
                return budget_call(
                    1, static_cast<std::uint64_t>(task), static_cast<std::uint64_t>(tokens),
                    static_cast<std::uint64_t>(wall), static_cast<std::uint64_t>(failures));
            }

            if (op == "publish-metric") {
                std::int64_t key = 0, value = 0;
                if (a.size() < 4 || !bus_int(a[2], &key) || !bus_int(a[3], &value))
                    return bus_error(ev, "validation-failed", "publish-metric needs key and value");
                aura::bus::LoanBody loan{};
                loan.channel = 1;
                loan.length = sizeof(aura::bus::MetricRecord);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::Reserve, loan))
                    return bus_error(ev, "transport-failed", "reserve send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "reserve reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, eb.code == ENOSPC ? "image-full" : "loan-exhausted",
                                     eb.message);
                }
                aura::bus::LoanBody granted{};
                std::memcpy(&granted, body.data(), std::min(sizeof(granted), body.size()));
                auto* dst = static_cast<std::byte*>(att.base) + granted.offset;
                aura::bus::MetricRecord m{};
                m.key_sym = static_cast<std::uint64_t>(key);
                m.kind = 1;
                m.value = static_cast<std::uint64_t>(value);
                std::memcpy(dst, &m, sizeof(m));
                aura::bus::LoanBody pub{};
                pub.channel = 1;
                pub.offset = granted.offset;
                pub.length = sizeof(m);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::Publish, pub))
                    return bus_error(ev, "transport-failed", "publish send failed");
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "publish reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "validation-failed", eb.message);
                }
                aura::bus::ChannelBody cb{};
                std::memcpy(&cb, body.data(), std::min(sizeof(cb), body.size()));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(cb.generation)),
                                     make_int(static_cast<std::int64_t>(cb.count))});
            }


            if (op == "trial") {
                std::int64_t channel = 1, task = 0;
                if (a.size() >= 3 && !bus_int(a[2], &channel))
                    return bus_error(ev, "validation-failed", "trial channel must be an integer");
                if (a.size() >= 4 && !bus_int(a[3], &task))
                    return bus_error(ev, "validation-failed", "trial task id must be an integer");
                aura::bus::LoanBody loan{};
                loan.channel = static_cast<std::uint64_t>(channel);
                // #5401: the request's worldline field carries the task id.
                loan.worldline = static_cast<std::uint64_t>(task);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::Trial, loan))
                    return bus_error(ev, "transport-failed", "trial send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "trial reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                aura::bus::LoanBody granted{};
                std::memcpy(&granted, body.data(), std::min(sizeof(granted), body.size()));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(granted.worldline))});
            }

            if (op == "keep" || op == "rollback") {
                std::int64_t worldline = 0, node = 0, value = 0;
                if (a.size() < 3 || !bus_int(a[2], &worldline))
                    return bus_error(ev, "validation-failed", "worldline id required");
                if (a.size() >= 4)
                    (void)bus_int(a[3], &node);
                if (a.size() >= 5)
                    (void)bus_int(a[4], &value);
                aura::bus::LoanBody loan{};
                loan.channel = 1;
                loan.worldline = static_cast<std::uint64_t>(worldline);
                loan.offset = static_cast<std::uint64_t>(node);
                loan.length = static_cast<std::uint64_t>(value);
                const auto type =
                    op == "keep" ? aura::bus::FrameType::Keep : aura::bus::FrameType::Rollback;
                if (!send_loan(att.sock, att.session, type, loan))
                    return bus_error(ev, "transport-failed", "worldline send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "worldline reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    const std::string msg(eb.message, strnlen(eb.message, sizeof(eb.message)));
                    // #5379/#5378: the coordinator prefixes gate reasons;
                    // surface the specific code, not a blanket constraint.
                    std::string code = op == "keep" ? "constraint-rejected" : "permission";
                    if (msg.rfind("validation-failed:", 0) == 0)
                        code = "validation-failed";
                    else if (msg.rfind("canary-active:", 0) == 0)
                        code = "canary-active";
                    else if (msg.rfind("missing capability:", 0) == 0 ||
                             msg.rfind("channel not in", 0) == 0 ||
                             msg.rfind("capability expired", 0) == 0 ||
                             msg.rfind("capability is scoped", 0) == 0 ||
                             msg.rfind("worldline is owned", 0) == 0)
                        code = "permission";
                    return bus_error(ev, code, msg);
                }
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(worldline))});
            }


            if (op == "grant") {
                std::int64_t bits = 0, channels = 0, loans = 0, expires = 0, worldline = 0;
                std::int64_t task = 0;
                if (a.size() < 7 || !bus_int(a[2], &bits) || !bus_int(a[3], &channels) ||
                    !bus_int(a[4], &loans) || !bus_int(a[5], &expires) ||
                    !bus_int(a[6], &worldline))
                    return bus_error(ev, "validation-failed",
                                     "grant needs bits, channels, loans, expires, worldline");
                if (a.size() >= 8 && !bus_int(a[7], &task))
                    return bus_error(ev, "validation-failed", "grant task id must be an integer");
                aura::bus::GrantBody request{};
                request.caps.bits = static_cast<std::uint64_t>(bits);
                request.caps.channels = static_cast<std::uint64_t>(channels);
                request.caps.max_loans = static_cast<std::uint64_t>(loans);
                request.caps.expires_gen = static_cast<std::uint64_t>(expires);
                request.caps.worldline = static_cast<std::uint64_t>(worldline);
                request.caps.task = static_cast<std::uint64_t>(task);
                if (!aura::bus::send_frame(
                        att.sock, aura::bus::FrameType::Grant, att.session,
                        {reinterpret_cast<const std::byte*>(&request), sizeof(request)}))
                    return bus_error(ev, "transport-failed", "grant send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "grant reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                aura::bus::GrantReply reply{};
                std::memcpy(&reply, body.data(), std::min(sizeof(reply), body.size()));
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(reply.token))});
            }

            // #5377: activate a granted token — narrow this session's caps
            // to the token's effective (scoped) capability. The judge uses
            // the host's delegated keep+rollback on a single worldline.
            // (bus-call "grant-activate" sid token) -> (ok bits worldline)
            if (op == "grant-activate") {
                std::int64_t token = 0;
                if (a.size() < 3 || !bus_int(a[2], &token))
                    return bus_error(ev, "validation-failed", "grant-activate needs a token");
                aura::bus::LoanBody loan{};
                loan.channel = static_cast<std::uint64_t>(token);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::GrantActivate, loan))
                    return bus_error(ev, "transport-failed", "grant activate send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "grant activate reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                aura::bus::LoanBody ok{};
                std::memcpy(&ok, body.data(), std::min(sizeof(ok), body.size()));
                // Reflect the narrowed caps in the session's attach record.
                state->att.caps = ok.channel;
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(ok.channel)),
                                     make_int(static_cast<std::int64_t>(ok.worldline))});
            }

            if (op == "revoke") {
                std::int64_t token = 0;
                if (a.size() < 3 || !bus_int(a[2], &token))
                    return bus_error(ev, "validation-failed", "revoke needs a token");
                aura::bus::LoanBody loan{};
                loan.channel = static_cast<std::uint64_t>(token);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::Revoke, loan))
                    return bus_error(ev, "transport-failed", "revoke send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "revoke reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                return bus_list(ev, {bus_string(ev, "ok")});
            }

            if (op == "audit") {
                std::int64_t worldline = 0, actor = 0, since_seq = 0, task = 0;
                if (a.size() >= 3 && !bus_int(a[2], &worldline))
                    return bus_error(ev, "validation-failed", "audit worldline must be an integer");
                if (a.size() >= 4 && !bus_int(a[3], &actor))
                    return bus_error(ev, "validation-failed", "audit actor must be an integer");
                if (a.size() >= 5 && !bus_int(a[4], &since_seq))
                    return bus_error(ev, "validation-failed", "audit since-seq must be an integer");
                if (a.size() >= 6 && !bus_int(a[5], &task))
                    return bus_error(ev, "validation-failed", "audit task must be an integer");
                aura::bus::AuditQueryBody req{};
                req.worldline = static_cast<std::uint64_t>(worldline);
                req.actor = static_cast<std::uint64_t>(actor);
                req.since_seq = static_cast<std::uint64_t>(since_seq);
                req.reserved = static_cast<std::uint64_t>(task);
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::AuditQuery, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "audit send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "audit reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (body.size() < sizeof(aura::bus::AuditReply))
                    return bus_error(ev, "validation-failed", "malformed audit reply");
                aura::bus::AuditReply ar{};
                std::memcpy(&ar, body.data(), sizeof(ar));
                std::vector<EvalValue> rows;
                for (std::uint64_t i = 0; i < ar.count; ++i) {
                    const std::size_t at = sizeof(ar) + i * sizeof(aura::bus::AuditWire);
                    if (at + sizeof(aura::bus::AuditWire) > body.size())
                        break;
                    aura::bus::AuditWire w{};
                    std::memcpy(&w, body.data() + at, sizeof(w));
                    rows.push_back(bus_list(
                        ev, {make_int(static_cast<std::int64_t>(w.kind)),
                             make_int(static_cast<std::int64_t>(w.token)),
                             make_int(static_cast<std::int64_t>(w.worldline)),
                             make_int(static_cast<std::int64_t>(w.result)),
                             make_int(static_cast<std::int64_t>(w.version)),
                             make_int(static_cast<std::int64_t>(w.timestamp_ms)),
                             make_int(static_cast<std::int64_t>(w.seq)),
                             bus_string(
                                 ev, std::string(w.reason, strnlen(w.reason, sizeof(w.reason))))}));
                }
                EvalValue listed = make_void();
                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                    listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(ar.count)),
                                     make_int(static_cast<std::int64_t>(ar.truncated)), listed});
            }

            // #5397: the host's live define names. The node ids are
            // host-local, so a peer asks the host for the name it wants
            // to rewrite instead of guessing an id.
            if (op == "defines") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "defines needs a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::DefineQuery, att.session,
                                           {}))
                    return bus_error(ev, "transport-failed", "define query send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "define query reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (body.size() < sizeof(aura::bus::DefineReply))
                    return bus_error(ev, "validation-failed", "malformed define reply");
                aura::bus::DefineReply reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                std::vector<EvalValue> rows;
                for (std::uint64_t i = 0; i < reply.count; ++i) {
                    const std::size_t at = sizeof(reply) + i * sizeof(aura::bus::DefineWire);
                    if (at + sizeof(aura::bus::DefineWire) > body.size())
                        break;
                    aura::bus::DefineWire w{};
                    std::memcpy(&w, body.data() + at, sizeof(w));
                    rows.push_back(bus_list(
                        ev, {bus_string(ev, std::string(w.name, strnlen(w.name, sizeof(w.name)))),
                             make_int(static_cast<std::int64_t>(w.node))}));
                }
                EvalValue listed = make_void();
                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                    listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.count)),
                                     make_int(static_cast<std::int64_t>(reply.truncated)), listed});
            }

            // #5397: read back a committed string-table entry — a
            // candidate define's source. The judge measures the
            // candidate body on held-out data, so it must see the text
            // the proposer committed.
            if (op == "source-read") {
                std::int64_t sid = 0, index = 0;
                if (a.size() < 3 || !bus_int(a[1], &sid) || !bus_int(a[2], &index))
                    return bus_error(ev, "validation-failed",
                                     "source-read needs a session id and an index");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                const std::uint64_t idx = static_cast<std::uint64_t>(index);
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::SourceRead, att.session,
                                           {reinterpret_cast<const std::byte*>(&idx), sizeof(idx)}))
                    return bus_error(ev, "transport-failed", "source read send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "source read reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                // The reply is the text; hdr.len is its length (the
                // buffer is a fixed-size scratch, padded with zeros).
                const std::string text(reinterpret_cast<const char*>(body.data()),
                                       std::min<std::size_t>(hdr.len, body.size()));
                return bus_list(ev, {bus_string(ev, "ok"), bus_string(ev, text)});
            }

            // #5397: the reproduction package of one worldline. It
            // carries the five-tuple, the held-out windows, the policy
            // snapshot and the verdict, so the admission decision can
            // be re-run offline and must land on the same verdict.
            if (op == "repro") {
                std::int64_t sid = 0, wl = 0;
                if (a.size() < 3 || !bus_int(a[1], &sid) || !bus_int(a[2], &wl))
                    return bus_error(ev, "validation-failed",
                                     "repro needs a session id and a worldline");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                const std::uint64_t worldline = static_cast<std::uint64_t>(wl);
                if (!aura::bus::send_frame(
                        att.sock, aura::bus::FrameType::ReproQuery, att.session,
                        {reinterpret_cast<const std::byte*>(&worldline), sizeof(worldline)}))
                    return bus_error(ev, "transport-failed", "repro query send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "repro query reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                // #5408: the reply must be the package frame the host
                // sends, and hdr.len is its actual length (the scratch
                // buffer is always kBusMaxControlFrame bytes, so body.size()
                // cannot detect a short frame).
                if (hdr.type != static_cast<std::uint16_t>(aura::bus::FrameType::Credit))
                    return bus_error(ev, "validation-failed", "unexpected repro reply frame");
                if (hdr.len < sizeof(aura::bus::ReproPackage))
                    return bus_error(ev, "validation-failed", "malformed repro reply");
                aura::bus::ReproPackage pkg{};
                std::memcpy(&pkg, body.data(), sizeof(pkg));
                // #5408: hashes are raw 64-bit values; a fixnum cannot hold
                // them, and make_int() would abort once production arms the
                // hot contract. Keep the low immediate bits in the list
                // (the JSON line below keeps the full value).
                constexpr std::uint64_t kImm = (1ull << 62) - 1;
                EvalValue five = make_void();
                if (pkg.has_five != 0) {
                    five = bus_list(
                        ev,
                        {make_int(static_cast<std::int64_t>(pkg.five.baseline_gen & kImm)),
                         make_int(static_cast<std::int64_t>(pkg.five.candidate_hash & kImm)),
                         make_int(static_cast<std::int64_t>(pkg.five.workload_sym & kImm)),
                         make_int(static_cast<std::int64_t>(pkg.five.seed & kImm)),
                         make_int(static_cast<std::int64_t>(pkg.five.verifier_version & kImm))});
                }
                EvalValue windows = make_void();
                if (pkg.has_gate != 0) {
                    const std::uint64_t n =
                        std::min<std::uint64_t>(pkg.gate.fold_count, aura::bus::kGateMaxWindows);
                    for (std::uint64_t i = n; i > 0; --i) {
                        const std::uint64_t raw = pkg.gate.windows[i - 1];
                        windows = make_pair(static_cast<std::uint64_t>(ev.push_pair(
                            types::make_int_or_promote(static_cast<std::int64_t>(raw)), windows)));
                    }
                }
                EvalValue policy = make_void();
                if (pkg.has_gate != 0) {
                    policy = bus_list(
                        ev, {make_int(static_cast<std::int64_t>(pkg.gate.round_id)),
                             make_int(static_cast<std::int64_t>(pkg.gate.threshold_ppm)),
                             make_int(static_cast<std::int64_t>(pkg.gate.control)),
                             make_int(static_cast<std::int64_t>(pkg.policy.min_windows)),
                             make_int(static_cast<std::int64_t>(pkg.policy.budget_window)),
                             make_int(static_cast<std::int64_t>(pkg.policy.tighten_factor)),
                             make_int(static_cast<std::int64_t>(pkg.policy.version))});
                }
                EvalValue verdict = make_void();
                if (pkg.has_verdict != 0) {
                    verdict = bus_list(
                        ev, {make_int(static_cast<std::int64_t>(pkg.verdict.verdict)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.family)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.observed_ppm)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.adjusted_ppm)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.haircut_ppm)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.threshold_ppm)),
                             make_int(static_cast<std::int64_t>(pkg.verdict.fingerprint & kImm))});
                }
                // The package is also rendered as one JSON line, so a
                // nightly can archive it verbatim and re-run the
                // decision offline (`aura bus-repro FILE`).
                std::string json =
                    "{\"v\":1,\"worldline\":" + std::to_string(pkg.gate.worldline) +
                    ",\"baseline_gen\":" + std::to_string(pkg.five.baseline_gen) +
                    ",\"candidate_hash\":" + std::to_string(pkg.five.candidate_hash) +
                    ",\"workload_sym\":" + std::to_string(pkg.five.workload_sym) +
                    ",\"seed\":" + std::to_string(pkg.five.seed) +
                    ",\"verifier_version\":" + std::to_string(pkg.five.verifier_version) +
                    ",\"round_id\":" + std::to_string(pkg.gate.round_id) +
                    ",\"fold_count\":" + std::to_string(pkg.gate.fold_count) +
                    ",\"threshold_ppm\":" + std::to_string(pkg.gate.threshold_ppm) +
                    ",\"control\":" + std::to_string(pkg.gate.control) + ",\"windows\":[";
                {
                    const std::uint64_t n =
                        std::min<std::uint64_t>(pkg.gate.fold_count, aura::bus::kGateMaxWindows);
                    for (std::uint64_t i = 0; i < n; ++i) {
                        if (i != 0)
                            json += ",";
                        const std::uint64_t raw = pkg.gate.windows[i];
                        json += std::to_string(static_cast<std::int64_t>(raw));
                    }
                }
                json +=
                    "],\"policy\":{\"threshold_ppm\":" + std::to_string(pkg.policy.threshold_ppm) +
                    ",\"min_windows\":" + std::to_string(pkg.policy.min_windows) +
                    ",\"budget_window\":" + std::to_string(pkg.policy.budget_window) +
                    ",\"tighten_factor\":" + std::to_string(pkg.policy.tighten_factor) +
                    ",\"version\":" + std::to_string(pkg.policy.version) +
                    // #5397: the package says whether the coordinator
                    // actually recorded a verdict. A worldline kept
                    // without the statistical gate has none, and the
                    // offline re-run must not compare one it never
                    // recorded.
                    "},\"has_verdict\":" + std::to_string(pkg.has_verdict) +
                    ",\"verdict\":{\"verdict\":" + std::to_string(pkg.verdict.verdict) +
                    ",\"family\":" + std::to_string(pkg.verdict.family) +
                    ",\"observed_ppm\":" + std::to_string(pkg.verdict.observed_ppm) +
                    ",\"adjusted_ppm\":" + std::to_string(pkg.verdict.adjusted_ppm) +
                    ",\"haircut_ppm\":" + std::to_string(pkg.verdict.haircut_ppm) +
                    ",\"threshold_ppm\":" + std::to_string(pkg.verdict.threshold_ppm) +
                    ",\"fingerprint\":" + std::to_string(pkg.verdict.fingerprint) +
                    ",\"policy_version\":" + std::to_string(pkg.verdict.policy_version) +
                    ",\"min_windows\":" + std::to_string(pkg.verdict.min_windows) +
                    ",\"tighten_factor\":" + std::to_string(pkg.verdict.tighten_factor) +
                    ",\"windows\":" + std::to_string(pkg.verdict.windows) + "}}";
                return bus_list(ev, {bus_string(ev, "ok"), five, windows, policy, verdict,
                                     bus_string(ev, json)});
            }

            // #5393: the admission round state machine. A peer asks
            // the coordinator to open a round; the host decides. The
            // state and the budget are queries.
            if (op == "round-open" || op == "round-state" || op == "round-budget") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "round verbs need a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                aura::bus::RoundRequestBody req{};
                req.kind = op == "round-budget" ? 1 : 0;
                const auto type = op == "round-open" ? aura::bus::FrameType::RoundOpen
                                                     : aura::bus::FrameType::RoundQuery;
                if (!aura::bus::send_frame(att.sock, type, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "round request send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "round reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::RoundReplyBody))
                    return bus_error(ev, "validation-failed", "malformed round reply");
                aura::bus::RoundReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                const std::string reason(reply.reason, strnlen(reply.reason, sizeof(reply.reason)));
                if (op == "round-open")
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(reply.round_id)),
                                         make_int(static_cast<std::int64_t>(reply.phase))});
                if (op == "round-budget")
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(reply.wall_ms)),
                                         make_int(static_cast<std::int64_t>(reply.cooldown_ms)),
                                         make_int(static_cast<std::int64_t>(reply.max_rounds)),
                                         make_int(static_cast<std::int64_t>(reply.version))});
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.phase)),
                                     make_int(static_cast<std::int64_t>(reply.round_id)),
                                     make_int(static_cast<std::int64_t>(reply.worldline)),
                                     make_int(static_cast<std::int64_t>(reply.opened)),
                                     make_int(static_cast<std::int64_t>(reply.sealed)),
                                     bus_string(ev, reason)});
            }

            // #5394: the degradation detector — its state, and the
            // versioned configuration (install or roll back).
            if (op == "degradation-state" || op == "degrade-install" || op == "degrade-rollback") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "degrade verbs need a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                if (op == "degradation-state") {
                    aura::bus::DegradeQueryBody req{};
                    if (!aura::bus::send_frame(
                            att.sock, aura::bus::FrameType::DegradeQuery, att.session,
                            {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                        return bus_error(ev, "transport-failed", "degrade query send failed");
                    aura::bus::FrameHeader hdr{};
                    std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                    if (!recv_reply(att.sock, &hdr, &body))
                        return bus_error(ev, "transport-failed", "degrade query reply lost");
                    if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                        aura::bus::ErrorBody eb{};
                        std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                        return bus_error(ev, "permission", eb.message);
                    }
                    if (hdr.len < sizeof(aura::bus::DegradeReplyBody))
                        return bus_error(ev, "validation-failed", "malformed degrade reply");
                    aura::bus::DegradeReplyBody reply{};
                    std::memcpy(&reply, body.data(), sizeof(reply));
                    const std::uint64_t count =
                        (hdr.len - sizeof(reply)) / sizeof(aura::bus::DegradeKeyStateBody);
                    std::vector<EvalValue> rows;
                    for (std::uint64_t i = 0; i < count; ++i) {
                        aura::bus::DegradeKeyStateBody key{};
                        std::memcpy(&key, body.data() + sizeof(reply) + i * sizeof(key),
                                    sizeof(key));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(key.key)),
                                          make_int(static_cast<std::int64_t>(key.higher_is_worse)),
                                          make_int(static_cast<std::int64_t>(key.samples)),
                                          make_int(static_cast<std::int64_t>(key.alerts)),
                                          make_int(key.ewma), make_int(key.cusum_ppm)}));
                    }
                    EvalValue listed = make_void();
                    for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                        listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(reply.version)),
                                         make_int(static_cast<std::int64_t>(reply.samples)),
                                         make_int(static_cast<std::int64_t>(reply.alerts)),
                                         make_int(static_cast<std::int64_t>(reply.last_change)),
                                         make_int(static_cast<std::int64_t>(reply.min_samples)),
                                         make_int(static_cast<std::int64_t>(reply.drift_ppm)),
                                         make_int(static_cast<std::int64_t>(reply.limit_ppm)),
                                         make_int(static_cast<std::int64_t>(reply.calm_samples)),
                                         listed});
                }
                // The configuration: install a new version or roll the
                // previous one back.
                aura::bus::DegradeConfigBody config{};
                config.flags = op == "degrade-rollback" ? 1 : 0;
                if (op == "degrade-install") {
                    std::int64_t min_samples = 0, alpha = 0, drift = 0, limit = 0, calm = 0;
                    if (a.size() < 7 || !bus_int(a[2], &min_samples) || !bus_int(a[3], &alpha) ||
                        !bus_int(a[4], &drift) || !bus_int(a[5], &limit) || !bus_int(a[6], &calm))
                        return bus_error(
                            ev, "validation-failed",
                            "degrade-install needs min-samples alpha drift limit calm keys");
                    config.min_samples = static_cast<std::uint64_t>(min_samples);
                    config.ewma_alpha_ppm = static_cast<std::uint64_t>(alpha);
                    config.drift_ppm = static_cast<std::uint64_t>(drift);
                    config.limit_ppm = static_cast<std::uint64_t>(limit);
                    config.calm_samples = static_cast<std::uint64_t>(calm);
                    // The keys: a list of key ids, and the ones where a
                    // rise is the degradation.
                    const auto pairs = ev.pairs();
                    const auto read_keys = [&](std::size_t index,
                                               std::vector<aura::bus::DegradeKeyBody>* out) {
                        EvalValue cur = index < a.size() ? a[index] : make_void();
                        while (is_pair(cur) && out->size() < 16) {
                            const auto idx = as_pair_idx(cur);
                            if (idx >= pairs.size())
                                break;
                            std::int64_t key = 0;
                            if (!bus_int(pairs[idx].car, &key))
                                break;
                            aura::bus::DegradeKeyBody wire{};
                            wire.key = static_cast<std::uint64_t>(key);
                            out->push_back(wire);
                            cur = pairs[idx].cdr;
                        }
                    };
                    std::vector<aura::bus::DegradeKeyBody> down;
                    std::vector<aura::bus::DegradeKeyBody> up;
                    read_keys(7, &down);
                    read_keys(8, &up);
                    config.key_count = down.size() + up.size();
                    std::vector<std::byte> payload(
                        sizeof(config) + config.key_count * sizeof(aura::bus::DegradeKeyBody));
                    std::memcpy(payload.data(), &config, sizeof(config));
                    std::size_t at = sizeof(config);
                    for (const auto& key : down) {
                        std::memcpy(payload.data() + at, &key, sizeof(key));
                        at += sizeof(key);
                    }
                    for (const auto& key : up) {
                        aura::bus::DegradeKeyBody wire = key;
                        wire.higher_is_worse = 1;
                        std::memcpy(payload.data() + at, &wire, sizeof(wire));
                        at += sizeof(wire);
                    }
                    if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::DegradeConfig,
                                               att.session, payload))
                        return bus_error(ev, "transport-failed", "degrade config send failed");
                } else {
                    if (!aura::bus::send_frame(
                            att.sock, aura::bus::FrameType::DegradeConfig, att.session,
                            {reinterpret_cast<const std::byte*>(&config), sizeof(config)}))
                        return bus_error(ev, "transport-failed", "degrade rollback send failed");
                }
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "degrade config reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::DegradeReplyBody))
                    return bus_error(ev, "validation-failed", "malformed degrade reply");
                aura::bus::DegradeReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(reply.version))});
            }

            // #5400: the supervisor's scaling query. The host measures
            // its own slack; the reply is the worker count it fits.
            if (op == "supervise") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "supervise needs a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                aura::bus::SupervisorQueryBody req{};
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::SupervisorQuery,
                                           att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "supervise query send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "supervise query reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::SupervisorReplyBody))
                    return bus_error(ev, "validation-failed", "malformed supervise reply");
                aura::bus::SupervisorReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.workers)),
                                     make_int(static_cast<std::int64_t>(reply.cpu_free_pct)),
                                     make_int(static_cast<std::int64_t>(reply.mem_free_mb)),
                                     make_int(static_cast<std::int64_t>(reply.reason))});
            }

            // #5395: the bus's own performance counters and the
            // capacity plan.
            if (op == "slo" || op == "capacity-plan") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "slo needs a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                aura::bus::SloQueryBody req{};
                req.kind = op == "capacity-plan" ? 1 : 0;
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::SloQuery, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "slo query send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "slo query reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::SloReplyBody))
                    return bus_error(ev, "validation-failed", "malformed slo reply");
                aura::bus::SloReplyBody reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                if (op == "slo")
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(reply.frames)),
                                         make_int(static_cast<std::int64_t>(reply.frame_cpu_ns)),
                                         make_int(static_cast<std::int64_t>(reply.p99_cpu_ns)),
                                         make_int(static_cast<std::int64_t>(reply.publishes)),
                                         make_int(static_cast<std::int64_t>(reply.publish_cpu_ns)),
                                         make_int(static_cast<std::int64_t>(reply.attaches)),
                                         make_int(static_cast<std::int64_t>(reply.attach_cpu_ns)),
                                         make_int(static_cast<std::int64_t>(reply.catchups)),
                                         make_int(static_cast<std::int64_t>(reply.catchup_max)),
                                         make_int(static_cast<std::int64_t>(reply.loans)),
                                         make_int(static_cast<std::int64_t>(reply.pending))});
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(reply.rounds)),
                                     make_int(static_cast<std::int64_t>(reply.low)),
                                     make_int(static_cast<std::int64_t>(reply.remaining[0])),
                                     make_int(static_cast<std::int64_t>(reply.remaining[1])),
                                     make_int(static_cast<std::int64_t>(reply.remaining[2])),
                                     make_int(static_cast<std::int64_t>(reply.remaining[3])),
                                     make_int(static_cast<std::int64_t>(reply.remaining[4])),
                                     make_int(static_cast<std::int64_t>(reply.committed_bytes)),
                                     make_int(static_cast<std::int64_t>(reply.image_bytes))});
            }

            // #5392: replay the audit log — the reconstructed lineage,
            // the integrity findings and the reproduced verdicts. The
            // evo export renders the same lineage as evo schema v2
            // JSONL, so `aura evo log` reads the bus lineage directly.
            if (op == "audit-replay" || op == "audit-evo") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "audit-replay needs a session id");
                const auto sit = g_bus_sessions.find(sid);
                if (sit == g_bus_sessions.end())
                    return bus_error(ev, "unregistered-id", "unknown session");
                auto& att = sit->second->att;
                aura::bus::AuditReplayBody req{};
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::AuditReplay, att.session,
                                           {reinterpret_cast<const std::byte*>(&req), sizeof(req)}))
                    return bus_error(ev, "transport-failed", "audit replay send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "audit replay reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                if (hdr.len < sizeof(aura::bus::AuditReplayReply))
                    return bus_error(ev, "validation-failed", "malformed audit replay reply");
                aura::bus::AuditReplayReply reply{};
                std::memcpy(&reply, body.data(), sizeof(reply));
                const std::uint64_t count =
                    (hdr.len - sizeof(reply)) / sizeof(aura::bus::AuditReplayNode);
                if (op == "audit-evo") {
                    std::string text;
                    for (std::uint64_t i = 0; i < count; ++i) {
                        aura::bus::AuditReplayNode node{};
                        std::memcpy(&node, body.data() + sizeof(reply) + i * sizeof(node),
                                    sizeof(node));
                        const std::string id = "w" + std::to_string(node.worldline);
                        const std::string parent =
                            node.parent != 0 ? "w" + std::to_string(node.parent) : std::string();
                        const char* decision = "TRIAL";
                        switch (node.state) {
                            case 1:
                                decision = "KEEP";
                                break;
                            case 2:
                                decision = "ROLLBACK";
                                break;
                            case 3:
                                decision = "REJECT";
                                break;
                            case 4:
                                decision = "CANARY";
                                break;
                            default:
                                break;
                        }
                        std::string reason;
                        aura::json::escape_into(
                            reason,
                            std::string(node.reason, strnlen(node.reason, sizeof(node.reason))));
                        text +=
                            "{\"v\":2,\"id\":\"" + id + "\",\"parent\":\"" + parent +
                            "\",\"run\":\"bus-" + std::to_string(reply.run) +
                            "\",\"seq\":" + std::to_string(node.trial_seq) +
                            ",\"ts_ms\":" + std::to_string(node.timestamp_ms) +
                            ",\"kind\":\"bus:worldline\",\"target\":\"" +
                            std::to_string(node.worldline) + "\",\"decision\":\"" + decision +
                            "\",\"reason\":\"" + reason + "\",\"evidence\":{\"baseline_gen\":" +
                            std::to_string(node.baseline_gen) +
                            ",\"candidate_hash\":" + std::to_string(node.candidate_hash) +
                            ",\"workload_sym\":" + std::to_string(node.workload_sym) +
                            ",\"seed\":" + std::to_string(node.seed) +
                            ",\"verifier_version\":" + std::to_string(node.verifier_version) +
                            ",\"gate_verdict\":" + std::to_string(node.gate_verdict) +
                            ",\"gate_family\":" + std::to_string(node.gate_family) +
                            ",\"adjusted_ppm\":" + std::to_string(node.adjusted_ppm) +
                            ",\"fingerprint\":" + std::to_string(node.fingerprint) +
                            "},\"old\":null,\"new\":null,\"dur_ms\":0,\"algo\":\"bus\",\"gen\":0,"
                            "\"slot\":0,\"search\":\"bus\",\"candidate\":\"" +
                            id + "\",\"parents\":[\"" + parent +
                            "\"],\"behavior\":null,\"objectives\":null,\"constraints\":null}\n";
                    }
                    return bus_list(ev, {bus_string(ev, "ok"), bus_string(ev, text)});
                }
                std::vector<EvalValue> rows;
                for (std::uint64_t i = 0; i < count; ++i) {
                    aura::bus::AuditReplayNode node{};
                    std::memcpy(&node, body.data() + sizeof(reply) + i * sizeof(node),
                                sizeof(node));
                    rows.push_back(bus_list(
                        ev,
                        {make_int(static_cast<std::int64_t>(node.worldline)),
                         make_int(static_cast<std::int64_t>(node.parent)),
                         make_int(static_cast<std::int64_t>(node.state)),
                         make_int(static_cast<std::int64_t>(node.trial_seq)),
                         make_int(static_cast<std::int64_t>(node.keep_seq)),
                         make_int(static_cast<std::int64_t>(node.verdict_seq)),
                         make_int(static_cast<std::int64_t>(node.constraint)),
                         make_int(static_cast<std::int64_t>(node.reproduced)),
                         make_int(static_cast<std::int64_t>(node.fingerprint)),
                         make_int(static_cast<std::int64_t>(node.timestamp_ms)),
                         make_int(static_cast<std::int64_t>(node.baseline_gen)),
                         make_int(static_cast<std::int64_t>(node.candidate_hash)),
                         make_int(static_cast<std::int64_t>(node.workload_sym)),
                         make_int(static_cast<std::int64_t>(node.seed)),
                         make_int(static_cast<std::int64_t>(node.verifier_version)),
                         make_int(static_cast<std::int64_t>(node.gate_verdict)),
                         make_int(static_cast<std::int64_t>(node.gate_family)),
                         make_int(node.adjusted_ppm),
                         bus_string(ev, std::string(node.reason,
                                                    strnlen(node.reason, sizeof(node.reason))))}));
                }
                EvalValue listed = make_void();
                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                    listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(reply.events)),
                         make_int(static_cast<std::int64_t>(reply.worldlines)),
                         make_int(static_cast<std::int64_t>(reply.keeps)),
                         make_int(static_cast<std::int64_t>(reply.findings)),
                         make_int(static_cast<std::int64_t>(reply.first_seq)),
                         make_int(static_cast<std::int64_t>(reply.first_kind)),
                         bus_string(ev, std::string(reply.first_reason,
                                                    strnlen(reply.first_reason,
                                                            sizeof(reply.first_reason)))),
                         listed});
            }


            if (op == "publish-cct" || op == "publish-patch" || op == "publish-worldline") {
                if (op == "publish-cct") {
                    std::int64_t parent = 0, mid = 0, node = 0, samples = 0, self_ns = 0;
                    if (a.size() < 7 || !bus_int(a[2], &parent) || !bus_int(a[3], &mid) ||
                        !bus_int(a[4], &node) || !bus_int(a[5], &samples) ||
                        !bus_int(a[6], &self_ns))
                        return bus_error(ev, "validation-failed", "publish-cct arguments");
                    aura::bus::CctRecord rec{};
                    rec.parent = static_cast<std::uint64_t>(parent);
                    rec.mid_lo = static_cast<std::uint64_t>(mid);
                    rec.node = static_cast<std::uint64_t>(node);
                    rec.samples = static_cast<std::uint64_t>(samples);
                    rec.self_ns = static_cast<std::uint64_t>(self_ns);
                    return bus_publish_record(ev, *state, 2, &rec, sizeof(rec));
                }
                if (op == "publish-patch") {
                    std::int64_t node = 0, field = 0, value = 0, worldline = 0, generation = 0;
                    if (a.size() < 7 || !bus_int(a[2], &node) || !bus_int(a[3], &field) ||
                        !bus_int(a[4], &value) || !bus_int(a[5], &worldline) ||
                        !bus_int(a[6], &generation))
                        return bus_error(ev, "validation-failed", "publish-patch arguments");
                    aura::bus::PatchRecord rec{};
                    rec.node = static_cast<std::uint64_t>(node);
                    rec.field_offset = static_cast<std::uint32_t>(field);
                    rec.new_value = static_cast<std::uint64_t>(value);
                    rec.worldline = static_cast<std::uint64_t>(worldline);
                    rec.generation = static_cast<std::uint64_t>(generation);
                    return bus_publish_record(ev, *state, 3, &rec, sizeof(rec));
                }
                std::int64_t id = 0, parent = 0, state_v = 0, constraint = 0, patch_off = 0,
                             patch_len = 0;
                if (a.size() < 8 || !bus_int(a[2], &id) || !bus_int(a[3], &parent) ||
                    !bus_int(a[4], &state_v) || !bus_int(a[5], &constraint) ||
                    !bus_int(a[6], &patch_off) || !bus_int(a[7], &patch_len))
                    return bus_error(ev, "validation-failed", "publish-worldline arguments");
                aura::bus::WorldlineRecord rec{};
                rec.id = static_cast<std::uint64_t>(id);
                rec.parent = static_cast<std::uint64_t>(parent);
                rec.state = static_cast<std::uint8_t>(state_v);
                rec.constraint_result = static_cast<std::uint8_t>(constraint);
                rec.patch_off = static_cast<std::uint64_t>(patch_off);
                rec.patch_len = static_cast<std::uint64_t>(patch_len);
                return bus_publish_record(ev, *state, 4, &rec, sizeof(rec));
            }

            // #5378: publish a candidate define source into the image's
            // string table; returns (ok index generation). The proposer
            // then publishes a patch with field=kPatchSourceField and
            // value=index naming the target define node.
            if (op == "publish-source") {
                if (a.size() < 3 || !is_string(a[2]))
                    return bus_error(ev, "validation-failed",
                                     "publish-source needs a source string");
                const std::string text = ev.string_at_or(as_string_idx(a[2]));
                if (text.size() > aura::bus::kBusMaxControlFrame)
                    return bus_error(ev, "validation-failed",
                                     "source text exceeds the control frame");
                if (!aura::bus::send_frame(
                        att.sock, aura::bus::FrameType::SourcePublish, att.session,
                        {reinterpret_cast<const std::byte*>(text.data()), text.size()}))
                    return bus_error(ev, "transport-failed", "source publish send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "source publish reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, eb.code == ENOSPC ? "image-full" : "permission",
                                     eb.message);
                }
                aura::bus::LoanBody ok{};
                std::memcpy(&ok, body.data(), std::min(sizeof(ok), body.size()));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(ok.offset)),
                                     make_int(static_cast<std::int64_t>(ok.generation))});
            }

            // #5379: bind the evidence five-tuple to a pending trial.
            // (bus-call "bind-evidence" sid wl baseline candidate workload seed verifier)
            if (op == "bind-evidence") {
                std::int64_t wl = 0, baseline = 0, candidate = 0, workload = 0, seed = 0,
                             verifier = 0;
                if (a.size() < 8 || !bus_int(a[2], &wl) || !bus_int(a[3], &baseline) ||
                    !bus_int(a[4], &candidate) || !bus_int(a[5], &workload) ||
                    !bus_int(a[6], &seed) || !bus_int(a[7], &verifier))
                    return bus_error(
                        ev, "validation-failed",
                        "bind-evidence needs wl baseline candidate workload seed verifier");
                aura::bus::EvidenceBody evb{};
                evb.worldline = static_cast<std::uint64_t>(wl);
                evb.baseline_gen = static_cast<std::uint64_t>(baseline);
                evb.candidate_hash = static_cast<std::uint64_t>(candidate);
                evb.workload_sym = static_cast<std::uint64_t>(workload);
                evb.seed = static_cast<std::uint64_t>(seed);
                evb.verifier_version = static_cast<std::uint64_t>(verifier);
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::EvidenceBind,
                                           att.session,
                                           {reinterpret_cast<const std::byte*>(&evb), sizeof(evb)}))
                    return bus_error(ev, "transport-failed", "evidence bind send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "evidence bind reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                return bus_list(ev, {bus_string(ev, "ok"), make_int(wl)});
            }

            // #5379: read a trial's bound evidence five-tuple.
            // (bus-call "evidence" sid wl) -> (ok baseline candidate workload seed verifier)
            if (op == "evidence") {
                std::int64_t wl = 0;
                if (a.size() < 3 || !bus_int(a[2], &wl))
                    return bus_error(ev, "validation-failed", "evidence needs a worldline id");
                // Reader-side: the evidence tail lives in the worldline
                // record; find it in the mapped image (no control frame).
                if ((state->att.caps & aura::bus::kCapRead) == 0)
                    return bus_error(ev, "permission", "missing capability: read");
                aura::bus::ChannelView view{};
                if (!aura::bus::channel_view(state->att.view(), aura::bus::Channel::Worldlines,
                                             &view))
                    return bus_error(ev, "unavailable", "worldline section is missing");
                const auto* base = static_cast<const std::byte*>(state->att.base);
                // The ledger appends a record per state transition; the
                // LAST record for the id carries the final evidence tail.
                aura::bus::WorldlineRecord found{};
                bool have = false;
                for (std::uint64_t i = 0; i < view.count; ++i) {
                    aura::bus::WorldlineRecord w{};
                    std::memcpy(&w, base + view.offset + i * view.element_size,
                                std::min<std::uint64_t>(view.element_size, sizeof(w)));
                    if (w.id == static_cast<std::uint64_t>(wl)) {
                        found = w;
                        have = true;
                    }
                }
                if (have) {
                    // Evidence fields are 62-bit immediates (#5379);
                    // mask defensively so a foreign record can never
                    // abort make_int's fixnum contract.
                    constexpr std::uint64_t kImm = (1ull << 62) - 1;
                    return bus_list(
                        ev, {bus_string(ev, "ok"),
                             make_int(static_cast<std::int64_t>(found.baseline_gen & kImm)),
                             make_int(static_cast<std::int64_t>(found.candidate_hash & kImm)),
                             make_int(static_cast<std::int64_t>(found.workload_sym & kImm)),
                             make_int(static_cast<std::int64_t>(found.seed & kImm)),
                             make_int(static_cast<std::int64_t>(found.verifier_version & kImm))});
                }
                return bus_error(ev, "unregistered-id", "worldline has no committed record");
            }

            // #5391: bind the judge's held-out evidence to a pending
            // trial. The coordinator recomputes the admission verdict
            // from the windows; the judge's own summary is audited, not
            // believed.
            // (bus-call "bind-gate" sid wl round workload seed verifier folds threshold control
            //                     verdict windows)
            if (op == "bind-gate") {
                std::int64_t wl = 0, round = 0, workload = 0, seed = 0, verifier = 0, folds = 0,
                             threshold = 0, control = 0, verdict = 0;
                if (a.size() < 12 || !bus_int(a[2], &wl) || !bus_int(a[3], &round) ||
                    !bus_int(a[4], &workload) || !bus_int(a[5], &seed) ||
                    !bus_int(a[6], &verifier) || !bus_int(a[7], &folds) ||
                    !bus_int(a[8], &threshold) || !bus_int(a[9], &control) ||
                    !bus_int(a[10], &verdict))
                    return bus_error(
                        ev, "validation-failed",
                        "bind-gate needs wl round workload seed verifier folds threshold control "
                        "verdict windows");
                aura::bus::GateEvidenceBody gev{};
                gev.worldline = static_cast<std::uint64_t>(wl);
                gev.round_id = static_cast<std::uint64_t>(round);
                gev.workload_sym = static_cast<std::uint64_t>(workload);
                gev.seed = static_cast<std::uint64_t>(seed);
                gev.verifier_version = static_cast<std::uint64_t>(verifier);
                gev.fold_count = static_cast<std::uint64_t>(folds);
                gev.threshold_ppm = static_cast<std::uint64_t>(threshold);
                gev.control = static_cast<std::uint64_t>(control);
                gev.verdict = static_cast<std::uint64_t>(verdict);
                // The held-out windows: a list of signed metric deltas.
                const auto pairs = ev.pairs();
                EvalValue cur = a[11];
                std::size_t n = 0;
                while (n < aura::bus::kGateMaxWindows && is_pair(cur)) {
                    const auto idx = as_pair_idx(cur);
                    if (idx >= pairs.size())
                        break;
                    std::int64_t w = 0;
                    if (!bus_int(pairs[idx].car, &w))
                        break;
                    gev.windows[n++] = static_cast<std::uint64_t>(w);
                    cur = pairs[idx].cdr;
                }
                if (n != static_cast<std::size_t>(folds) || folds <= 0)
                    return bus_error(ev, "validation-failed",
                                     "bind-gate window count does not match the fold count");
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::GateBind, att.session,
                                           {reinterpret_cast<const std::byte*>(&gev), sizeof(gev)}))
                    return bus_error(ev, "transport-failed", "gate bind send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "gate bind reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                return bus_list(ev, {bus_string(ev, "ok"), make_int(wl)});
            }

            // #5391: read the coordinator's admission verdict for a
            // worldline. (bus-call "gate-evidence" sid wl)
            // -> (ok verdict family adjusted threshold fingerprint)
            if (op == "gate-evidence") {
                std::int64_t wl = 0;
                if (a.size() < 3 || !bus_int(a[2], &wl))
                    return bus_error(ev, "validation-failed", "gate-evidence needs a worldline id");
                if ((state->att.caps & aura::bus::kCapRead) == 0)
                    return bus_error(ev, "permission", "missing capability: read");
                aura::bus::ChannelView view{};
                if (!aura::bus::channel_view(state->att.view(), aura::bus::Channel::Worldlines,
                                             &view))
                    return bus_error(ev, "unavailable", "worldline section is missing");
                const auto* base = static_cast<const std::byte*>(state->att.base);
                aura::bus::WorldlineRecord found{};
                bool have = false;
                for (std::uint64_t i = 0; i < view.count; ++i) {
                    aura::bus::WorldlineRecord w{};
                    std::memcpy(&w, base + view.offset + i * view.element_size,
                                std::min<std::uint64_t>(view.element_size, sizeof(w)));
                    if (w.id == static_cast<std::uint64_t>(wl)) {
                        found = w;
                        have = true;
                    }
                }
                if (!have)
                    return bus_error(ev, "unregistered-id", "worldline has no committed record");
                constexpr std::uint64_t kImm = (1ull << 62) - 1;
                const std::int64_t adjusted = static_cast<std::int64_t>(found.gate_adjusted_ppm) -
                                              static_cast<std::int64_t>(aura::bus::kGateBias);
                return bus_list(
                    ev,
                    {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(found.gate_verdict)),
                     make_int(static_cast<std::int64_t>(found.gate_family)), make_int(adjusted),
                     make_int(static_cast<std::int64_t>(found.gate_threshold_ppm & kImm)),
                     make_int(static_cast<std::int64_t>(found.gate_fingerprint & kImm))});
            }

            // #5379: read the coordinator's digest of a trial's patch
            // bytes, to bind as candidate_hash. (bus-call "candidate-digest" sid wl)
            if (op == "candidate-digest") {
                std::int64_t wl = 0;
                if (a.size() < 3 || !bus_int(a[2], &wl))
                    return bus_error(ev, "validation-failed",
                                     "candidate-digest needs a worldline id");
                aura::bus::LoanBody loan{};
                loan.worldline = static_cast<std::uint64_t>(wl);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::CandidateDigest, loan))
                    return bus_error(ev, "transport-failed", "candidate digest send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "candidate digest reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                aura::bus::LoanBody ok{};
                std::memcpy(&ok, body.data(), std::min(sizeof(ok), body.size()));
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(ok.offset))});
            }

            // #5381: seal the active canary early (judge confirmation).
            if (op == "canary-confirm") {
                std::int64_t wl = 0;
                if (a.size() < 3 || !bus_int(a[2], &wl))
                    return bus_error(ev, "validation-failed",
                                     "canary-confirm needs a worldline id");
                aura::bus::LoanBody loan{};
                loan.channel = 1;
                loan.worldline = static_cast<std::uint64_t>(wl);
                if (!send_loan(att.sock, att.session, aura::bus::FrameType::CanaryConfirm, loan))
                    return bus_error(ev, "transport-failed", "canary confirm send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> body(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &body))
                    return bus_error(ev, "transport-failed", "canary confirm reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, body.data(), std::min(sizeof(eb), body.size()));
                    return bus_error(ev, "permission", eb.message);
                }
                return bus_list(ev, {bus_string(ev, "ok"), make_int(wl)});
            }


            if (op == "project-nodes") {
                auto* flat = active_workspace(ev, *state);
                if (flat == nullptr)
                    return bus_error(ev, "unavailable", "no active workspace AST");
                const std::size_t count = std::min<std::size_t>(flat->size(), 4096);
                if (count == 0)
                    return bus_list(ev, {bus_string(ev, "ok"), make_int(0), make_int(0)});
                std::vector<aura::bus::NodeRecord> records;
                records.reserve(count);
                // Parent edges from the tree itself; the FlatAST id space
                // is not the batch index space, and the image has no symbol
                // table, so sym stays kImageNoIndex (validate_image checks
                // both on the next peer's attach — #5128 persistent image).
                std::vector<std::uint64_t> parent(count, aura::bus::kImageNoIndex);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto view = flat->get(static_cast<aura::ast::NodeId>(i));
                    for (const auto child : view.children)
                        if (child < count)
                            parent[child] = i;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    const auto view = flat->get(static_cast<aura::ast::NodeId>(i));
                    aura::bus::NodeRecord rec{};
                    rec.kind = static_cast<std::uint32_t>(view.tag);
                    rec.slot = static_cast<std::uint64_t>(view.id);
                    rec.parent = parent[i];
                    rec.first_child = aura::bus::kImageNoIndex;
                    for (const auto child : view.children)
                        if (child < count) {
                            rec.first_child = child;
                            break;
                        }
                    rec.next_sibling = aura::bus::kImageNoIndex;
                    if (parent[i] != aura::bus::kImageNoIndex) {
                        const auto pv = flat->get(static_cast<aura::ast::NodeId>(parent[i]));
                        bool seen = false;
                        for (const auto child : pv.children) {
                            if (child >= count)
                                continue;
                            if (seen) {
                                rec.next_sibling = child;
                                break;
                            }
                            if (child == i)
                                seen = true;
                        }
                    }
                    rec.sym = aura::bus::kImageNoIndex;
                    rec.src_line = view.line;
                    rec.src_col = view.col;
                    rec.src_file = 0;
                    records.push_back(rec);
                }
                return bus_publish_record(ev, *state, 5, records.data(),
                                          records.size() * sizeof(aura::bus::NodeRecord));
            }

            // Live patch producer: every committed mutation that carries
            // rollback data is a real patch of the live AST. The worldline
            // slot names the rebind in force when it happened, so a reader
            // can join patches to worldline ids without guessing.
            if (op == "project-patches") {
                auto* flat = active_workspace(ev, *state);
                if (flat == nullptr)
                    return bus_error(ev, "unavailable", "no active workspace AST");
                const auto log = flat->mutation_log_view();
                constexpr std::size_t kMax = 1024;
                std::vector<aura::bus::PatchRecord> records;
                records.reserve(std::min<std::size_t>(log.size(), kMax));
                std::vector<std::uint64_t> slots;
                std::uint64_t cursor = state->patches_cursor;
                std::uint64_t skipped = 0;
                for (const auto& rec : log) {
                    if (rec.mutation_id <= state->patches_cursor)
                        continue;
                    if (!rec.has_rollback_data || rec.target_node == aura::ast::NULL_NODE) {
                        cursor = rec.mutation_id;
                        continue;
                    }
                    if ((rec.new_value >> 63) != 0 || rec.target_node >= 4096) {
                        ++skipped;
                        cursor = rec.mutation_id;
                        continue;
                    }
                    if (records.size() >= kMax)
                        break;
                    aura::bus::PatchRecord p{};
                    p.node = static_cast<std::uint64_t>(rec.target_node);
                    p.field_offset = rec.field_offset;
                    p.new_value = rec.new_value;
                    p.worldline = worldline_slot_for(*state, log, rec.mutation_id);
                    records.push_back(p);
                    slots.push_back(p.worldline);
                    cursor = rec.mutation_id;
                }
                if (records.empty()) {
                    state->patches_cursor = cursor;
                    return bus_list(ev,
                                    {bus_string(ev, "ok"), make_int(0),
                                     make_int(static_cast<std::int64_t>(state->patches_published)),
                                     make_int(static_cast<std::int64_t>(cursor)),
                                     make_int(static_cast<std::int64_t>(skipped))});
                }
                std::uint64_t first = 0;
                bool published = false;
                EvalValue reply = bus_publish_record(
                    ev, *state, 3, records.data(), records.size() * sizeof(aura::bus::PatchRecord),
                    &first, &published);
                if (!published)
                    return reply;
                // Extend the per-worldline range; the records in one batch
                // are contiguous, so the section-relative span is exact.
                for (std::size_t i = 0; i < records.size(); ++i) {
                    if (slots[i] == aura::bus::kImageNoIndex)
                        continue;
                    const std::uint64_t rel =
                        section_relative(state->att, aura::bus::SectionKind::Patches,
                                         first + i * sizeof(aura::bus::PatchRecord));
                    auto& range = state->patch_range_by_slot[slots[i]];
                    if (range.second == 0)
                        range = {rel, sizeof(aura::bus::PatchRecord)};
                    else {
                        const std::uint64_t end = std::max(range.first + range.second,
                                                           rel + sizeof(aura::bus::PatchRecord));
                        range.first = std::min(range.first, rel);
                        range.second = end - range.first;
                    }
                }
                state->patches_cursor = cursor;
                state->patches_published += records.size();
                // (ok generation count next-cursor skipped)
                std::int64_t generation = 0;
                (void)bus_reply_int(ev, reply, 1, &generation);
                return bus_list(ev, {bus_string(ev, "ok"), make_int(generation),
                                     make_int(static_cast<std::int64_t>(state->patches_published)),
                                     make_int(static_cast<std::int64_t>(cursor)),
                                     make_int(static_cast<std::int64_t>(skipped))});
            }

            // Live worldline producer: one record per committed rebind, in
            // mutation order. id is the real mutation id, parent is the
            // parent mutation's worldline slot, and the patch range is the
            // span project-patches already wrote for that slot.
            if (op == "project-worldlines") {
                auto* flat = active_workspace(ev, *state);
                if (flat == nullptr)
                    return bus_error(ev, "unavailable", "no active workspace AST");
                const auto log = flat->mutation_log_view();
                constexpr std::size_t kMax = 1024;
                std::vector<aura::bus::WorldlineRecord> records;
                records.reserve(std::min<std::size_t>(log.size(), kMax));
                std::uint64_t cursor = state->worldlines_cursor;
                for (const auto& rec : log) {
                    if (rec.mutation_id <= state->worldlines_cursor)
                        continue;
                    if (!is_rebind_record(rec))
                        continue;
                    if (records.size() >= kMax)
                        break;
                    note_rebinds_through(*state, log, rec.mutation_id);
                    const std::uint64_t slot = state->wl_slot_by_mid.at(rec.mutation_id);
                    if (slot != state->worldlines_published + records.size())
                        return bus_error(ev, "validation-failed",
                                         "worldline slots published out of order");
                    aura::bus::WorldlineRecord w{};
                    w.id = rec.mutation_id;
                    w.parent = aura::bus::kImageNoParent;
                    if (rec.parent_mutation_id != 0) {
                        const auto pit = state->wl_slot_by_mid.find(rec.parent_mutation_id);
                        if (pit != state->wl_slot_by_mid.end() && pit->second < slot)
                            w.parent = pit->second;
                    }
                    w.state = rec.status == aura::ast::MutationStatus::Committed
                                  ? static_cast<std::uint8_t>(aura::bus::WorldlineState::Kept)
                                  : static_cast<std::uint8_t>(aura::bus::WorldlineState::Rolled);
                    w.constraint_result =
                        static_cast<std::uint8_t>(aura::bus::ConstraintResult::None);
                    w.metrics_off = aura::bus::kImageNoIndex;
                    const auto rit = state->patch_range_by_slot.find(slot);
                    if (rit != state->patch_range_by_slot.end()) {
                        w.patch_off = rit->second.first;
                        w.patch_len = rit->second.second;
                        w.patch_count = static_cast<std::uint32_t>(rit->second.second /
                                                                   sizeof(aura::bus::PatchRecord));
                    }
                    records.push_back(w);
                    cursor = rec.mutation_id;
                }
                if (records.empty()) {
                    state->worldlines_cursor = cursor;
                    return bus_list(
                        ev, {bus_string(ev, "ok"), make_int(0),
                             make_int(static_cast<std::int64_t>(state->worldlines_published)),
                             make_int(static_cast<std::int64_t>(cursor))});
                }
                bool published = false;
                EvalValue reply = bus_publish_record(
                    ev, *state, 4, records.data(),
                    records.size() * sizeof(aura::bus::WorldlineRecord), nullptr, &published);
                if (!published)
                    return reply;
                state->worldlines_cursor = cursor;
                state->worldlines_published += records.size();
                // (ok generation count next-cursor)
                std::int64_t generation = 0;
                (void)bus_reply_int(ev, reply, 1, &generation);
                return bus_list(ev,
                                {bus_string(ev, "ok"), make_int(generation),
                                 make_int(static_cast<std::int64_t>(state->worldlines_published)),
                                 make_int(static_cast<std::int64_t>(cursor))});
            }

            // Live CCT producer: project the call-site tree of the live
            // FlatAST. One record per Call node, parent = the enclosing
            // Call node, mid = the callee symbol; samples=1 marks one
            // static occurrence (no sampler is attached yet).
            if (op == "project-cct") {
                auto* flat = active_workspace(ev, *state);
                if (flat == nullptr)
                    return bus_error(ev, "unavailable", "no active workspace AST");
                // Live samples win over the static call-site fallback. The
                // recorder is thread-local, so a multi-fiber producer
                // publishes per fiber; a snapshot is published once and
                // re-requested calls report the same count.
                const auto& recorded = aura::cct::recorder();
                if (recorded.count > 0) {
                    if (state->cct_published)
                        return bus_list(ev, {bus_string(ev, "ok"),
                                             make_int(static_cast<std::int64_t>(
                                                 state->cct_static_live ? state->cct_static_count
                                                                        : recorded.count)),
                                             make_int(1)});
                    constexpr std::uint64_t kMidMask = (1ull << 62) - 1;
                    std::vector<aura::bus::CctRecord> live;
                    live.reserve(recorded.count);
                    for (std::uint32_t i = 0; i < recorded.count; ++i) {
                        const auto& n = recorded.nodes[i];
                        aura::bus::CctRecord c{};
                        c.parent = n.parent == aura::cct::kNoParent
                                       ? aura::bus::kImageNoParent
                                       : static_cast<std::uint64_t>(n.parent);
                        c.mid_lo = n.mid & kMidMask;
                        c.node = aura::bus::kImageNoIndex; // mid carries the body id
                        c.samples = n.samples;
                        c.self_ns = n.self_ns;
                        c.flags = 0; // live sample, not a static call site
                        live.push_back(c);
                    }
                    bool published = false;
                    EvalValue reply = bus_publish_record(ev, *state, 2, live.data(),
                                                         live.size() * sizeof(aura::bus::CctRecord),
                                                         nullptr, &published);
                    if (!published)
                        return reply;
                    state->cct_published = true;
                    state->cct_static_count = live.size();
                    state->cct_static_live = true;
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(live.size())),
                                         make_int(1)});
                }
                // #5386: same once-per-session contract as the live path —
                // a repeat call reports the published count, never doubles
                // the channel's records (and hotspot samples with them).
                if (state->cct_published)
                    return bus_list(ev,
                                    {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(state->cct_static_count)),
                                     make_int(1)});
                const std::size_t count = std::min<std::size_t>(flat->size(), 4096);
                if (count == 0)
                    return bus_list(ev, {bus_string(ev, "ok"), make_int(0), make_int(0)});
                std::vector<std::uint32_t> parent(count, aura::ast::NULL_NODE);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto view = flat->get(static_cast<aura::ast::NodeId>(i));
                    for (const auto child : view.children)
                        if (child < count)
                            parent[child] = static_cast<std::uint32_t>(i);
                }
                constexpr std::uint64_t kMidMask = (1ull << 62) - 1;
                std::vector<aura::bus::CctRecord> records;
                std::vector<std::int64_t> cct_index(count, -1);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto view = flat->get(static_cast<aura::ast::NodeId>(i));
                    if (view.tag != aura::ast::NodeTag::Call)
                        continue;
                    aura::bus::CctRecord c{};
                    c.parent = aura::bus::kImageNoParent;
                    std::uint64_t owner_define = aura::bus::kImageNoIndex;
                    std::uint32_t at = parent[i];
                    for (int steps = 0; steps < 64 && at != aura::ast::NULL_NODE && at < i;
                         ++steps) {
                        const auto ancestor = flat->get(static_cast<aura::ast::NodeId>(at));
                        if (owner_define == aura::bus::kImageNoIndex &&
                            ancestor.tag == aura::ast::NodeTag::Define)
                            owner_define = static_cast<std::uint64_t>(at);
                        if (cct_index[at] >= 0 && c.parent == aura::bus::kImageNoParent) {
                            c.parent = static_cast<std::uint64_t>(cct_index[at]);
                        }
                        at = parent[at];
                    }
                    std::uint64_t mid = 0;
                    if (!view.children.empty()) {
                        const auto callee = flat->get(view.children[0]);
                        if (callee.sym_id != aura::ast::INVALID_SYM)
                            mid = static_cast<std::uint64_t>(callee.sym_id);
                    }
                    c.mid_lo = mid & kMidMask;
                    // Align the record to the enclosing define so a rebind
                    // is the same node identity here, in PATCHES and in the
                    // worldline's patch range.
                    c.node = owner_define != aura::bus::kImageNoIndex
                                 ? owner_define
                                 : static_cast<std::uint64_t>(i);
                    c.samples = 1;
                    c.self_ns = 0;
                    c.flags = 1; // static call-site projection
                    cct_index[i] = static_cast<std::int64_t>(records.size());
                    records.push_back(c);
                    if (records.size() >= 1024)
                        break;
                }
                if (records.empty())
                    return bus_list(ev, {bus_string(ev, "ok"), make_int(0), make_int(0)});
                bool published = false;
                EvalValue reply = bus_publish_record(ev, *state, 2, records.data(),
                                                     records.size() * sizeof(aura::bus::CctRecord),
                                                     nullptr, &published);
                if (!published)
                    return reply;
                state->cct_published = true;
                state->cct_static_count = records.size();
                state->cct_static_live = false;
                return bus_list(ev,
                                {bus_string(ev, "ok"),
                                 make_int(static_cast<std::int64_t>(records.size())), make_int(0)});
            }

            // Agent query face (#5125): reader-side aggregations over the
            // mapped image. No control frame and no extra copy of the CCT.
            if (op == "constraints") {
                if ((state->att.caps & aura::bus::kCapRead) == 0)
                    return bus_error(ev, "permission", "missing capability: read");
                std::uint64_t version = 0;
                std::span<const std::byte> blob;
                if (!aura::bus::constraint_read(state->att.view(), &version, &blob))
                    return bus_error(ev, "unavailable", "constraints section is missing");
                std::vector<std::pair<std::string, std::string>> rules;
                std::size_t pos = 0;
                auto read_u32 = [&](std::uint32_t* out) {
                    if (pos + sizeof(std::uint32_t) > blob.size())
                        return false;
                    std::memcpy(out, blob.data() + pos, sizeof(*out));
                    pos += sizeof(*out);
                    return true;
                };
                while (pos < blob.size()) {
                    std::uint32_t name_len = 0, source_len = 0;
                    if (!read_u32(&name_len) || !read_u32(&source_len))
                        break;
                    if (name_len > blob.size() - pos || source_len > blob.size() - pos - name_len)
                        break;
                    std::string name(reinterpret_cast<const char*>(blob.data() + pos), name_len);
                    pos += name_len;
                    std::string source(reinterpret_cast<const char*>(blob.data() + pos),
                                       source_len);
                    pos += source_len;
                    rules.emplace_back(std::move(name), std::move(source));
                }
                EvalValue rows = make_void();
                for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
                    const EvalValue row =
                        bus_list(ev, {bus_string(ev, it->first), bus_string(ev, it->second)});
                    rows = make_pair(static_cast<std::uint64_t>(ev.push_pair(row, rows)));
                }
                return bus_list(ev,
                                {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(version)),
                                 make_int(static_cast<std::int64_t>(rules.size())), rows});
            }

            if (op == "install-constraint") {
                if (a.size() < 4 || !is_string(a[2]) || !is_string(a[3]))
                    return bus_error(ev, "validation-failed",
                                     "install-constraint needs name and source strings");
                const std::string name = ev.string_at_or(as_string_idx(a[2]));
                const std::string source = ev.string_at_or(as_string_idx(a[3]));
                if (name.size() > 240 || source.size() > 2048)
                    return bus_error(ev, "validation-failed", "constraint name/source too long");
                aura::bus::ConstraintWire wire{};
                wire.name_len = static_cast<std::uint32_t>(name.size());
                wire.source_len = static_cast<std::uint32_t>(source.size());
                std::vector<std::byte> body(sizeof(wire) + name.size() + source.size());
                std::memcpy(body.data(), &wire, sizeof(wire));
                std::memcpy(body.data() + sizeof(wire), name.data(), name.size());
                std::memcpy(body.data() + sizeof(wire) + name.size(), source.data(), source.size());
                if (!aura::bus::send_frame(att.sock, aura::bus::FrameType::ConstraintInstall,
                                           att.session, body))
                    return bus_error(ev, "transport-failed", "constraint install send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> reply(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &reply))
                    return bus_error(ev, "transport-failed", "constraint install reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, reply.data(), std::min(sizeof(eb), reply.size()));
                    return bus_error(ev, eb.code == EPERM ? "permission" : "validation-failed",
                                     eb.message);
                }
                aura::bus::LoanBody granted{};
                std::memcpy(&granted, reply.data(), std::min(sizeof(granted), reply.size()));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(granted.generation))});
            }

            if (op == "rollback-constraints") {
                aura::bus::LoanBody empty{};
                if (!aura::bus::send_frame(
                        att.sock, aura::bus::FrameType::ConstraintRollback, att.session,
                        {reinterpret_cast<const std::byte*>(&empty), sizeof(empty)}))
                    return bus_error(ev, "transport-failed", "constraint rollback send failed");
                aura::bus::FrameHeader hdr{};
                std::vector<std::byte> reply(aura::bus::kBusMaxControlFrame);
                if (!recv_reply(att.sock, &hdr, &reply))
                    return bus_error(ev, "transport-failed", "constraint rollback reply lost");
                if (hdr.type == static_cast<std::uint16_t>(aura::bus::FrameType::Error)) {
                    aura::bus::ErrorBody eb{};
                    std::memcpy(&eb, reply.data(), std::min(sizeof(eb), reply.size()));
                    return bus_error(ev, eb.code == EPERM ? "permission" : "validation-failed",
                                     eb.message);
                }
                return bus_list(ev, {bus_string(ev, "ok")});
            }

            if (op == "hotspots") {
                std::int64_t top = 0;
                if (a.size() < 3 || !bus_int(a[2], &top) || top < 0)
                    return bus_error(ev, "validation-failed", "hotspots needs a top count");
                if ((state->att.caps & aura::bus::kCapRead) == 0)
                    return bus_error(ev, "permission", "missing capability: read");
                std::vector<aura::bus::Hotspot> hotspots;
                std::string why;
                if (!aura::bus::cct_hotspots(state->att.view(), static_cast<std::size_t>(top),
                                             &hotspots, &why))
                    return bus_error(ev, "unavailable", why);
                EvalValue rows = make_void();
                for (auto it = hotspots.rbegin(); it != hotspots.rend(); ++it) {
                    const EvalValue row =
                        bus_list(ev, {make_int(static_cast<std::int64_t>(it->mid)),
                                      make_int(static_cast<std::int64_t>(it->samples)),
                                      make_int(static_cast<std::int64_t>(it->self_ns)),
                                      make_int(static_cast<std::int64_t>(it->node))});
                    rows = make_pair(static_cast<std::uint64_t>(ev.push_pair(row, rows)));
                }
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(hotspots.size())), rows});
            }

            if (op == "worldline-diff") {
                std::int64_t id_a = 0, id_b = 0;
                if (a.size() < 4 || !bus_int(a[2], &id_a) || !bus_int(a[3], &id_b))
                    return bus_error(ev, "validation-failed",
                                     "worldline-diff needs two worldline ids");
                if ((state->att.caps & aura::bus::kCapRead) == 0)
                    return bus_error(ev, "permission", "missing capability: read");
                aura::bus::WorldlineDiff diff;
                std::string why;
                if (!aura::bus::worldline_diff(state->att.view(), static_cast<std::uint64_t>(id_a),
                                               static_cast<std::uint64_t>(id_b), &diff, &why))
                    return bus_error(ev, "validation-failed", why);
                EvalValue rows = make_void();
                for (auto it = diff.rows.rbegin(); it != diff.rows.rend(); ++it) {
                    const EvalValue row =
                        bus_list(ev, {make_int(static_cast<std::int64_t>(it->node)),
                                      make_int(it->in_a ? 1 : 0), make_int(it->in_b ? 1 : 0),
                                      make_int(static_cast<std::int64_t>(it->field_a)),
                                      make_int(static_cast<std::int64_t>(it->field_b)),
                                      make_int(static_cast<std::int64_t>(it->value_a)),
                                      make_int(static_cast<std::int64_t>(it->value_b))});
                    rows = make_pair(static_cast<std::uint64_t>(ev.push_pair(row, rows)));
                }
                return bus_list(ev, {bus_string(ev, "ok"), make_int(diff.state_a),
                                     make_int(diff.state_b), rows});
            }

            return bus_error(ev, "unsupported", "unknown bus-call op");
        },
        pure_general(255, "(string any...) -> list",
                     "Bus session bridge: attach, detach, subscribe, query, publish-metric."));

    // g_tl_arena is thread-local TLarena declared in runtime_shared.h.
    // #5144: dispatch-only alias; reachable via (query :arena-offset).
    ev.primitives().add_hidden("query:arena-offset", [](const auto&) -> EvalValue {
        return make_int(static_cast<int64_t>(g_tl_arena.offset));
    });
}

} // namespace aura::compiler::primitives_detail
