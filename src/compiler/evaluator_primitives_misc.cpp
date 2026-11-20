// evaluator_primitives_misc.cpp — P0 step 28: current-time / arena-offset
// primitives; Issue #2655: current-time-ms + monotonic-ms for sub-second
// denseness metrology.
// Issue #2915: proof migration onto register_prim + PrimSpec scaffold.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "prim_registrar_scaffold.hh"

#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "bus/transport.hh"
#include "bus/query.hh"
#include "compiler/cct_probe.hh"

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
                return "unregistered-id";
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
                return bus_list(ev, {bus_string(ev, "ok"), make_int(sid),
                                     make_int(static_cast<std::int64_t>(size)),
                                     make_int(static_cast<std::int64_t>(granted))});
            }

            if (op == "detach") {
                std::int64_t sid = 0;
                if (a.size() < 2 || !bus_int(a[1], &sid))
                    return bus_error(ev, "validation-failed", "detach needs a session id");
                if (g_bus_sessions.erase(sid) == 0)
                    return bus_error(ev, "unregistered-id", "unknown session");
                return bus_list(ev, {bus_string(ev, "ok")});
            }

            BusSessionState* state = session(1);
            if (state == nullptr)
                return bus_error(ev, "unregistered-id", "unknown session");
            auto& att = state->att;

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
                    } else if (channel == 4 && elem >= sizeof(aura::bus::WorldlineRecord)) {
                        aura::bus::WorldlineRecord w{};
                        std::memcpy(&w, rec, sizeof(w));
                        rows.push_back(
                            bus_list(ev, {make_int(static_cast<std::int64_t>(w.id)),
                                          make_int(static_cast<std::int64_t>(w.parent)),
                                          make_int(w.state), make_int(w.constraint_result),
                                          make_int(static_cast<std::int64_t>(w.patch_off)),
                                          make_int(static_cast<std::int64_t>(w.patch_len))}));
                    }
                }
                EvalValue listed = make_void();
                for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                    listed = make_pair(static_cast<std::uint64_t>(ev.push_pair(*it, listed)));
                return bus_list(ev, {bus_string(ev, "ok"),
                                     make_int(static_cast<std::int64_t>(cb.generation)),
                                     make_int(static_cast<std::int64_t>(cb.count)),
                                     make_int(static_cast<std::int64_t>(cb.snapshot)), listed});
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
                std::int64_t channel = 1;
                if (a.size() >= 3 && !bus_int(a[2], &channel))
                    return bus_error(ev, "validation-failed", "trial channel must be an integer");
                aura::bus::LoanBody loan{};
                loan.channel = static_cast<std::uint64_t>(channel);
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
                    return bus_error(ev, op == "keep" ? "constraint-rejected" : "permission",
                                     eb.message);
                }
                return bus_list(
                    ev, {bus_string(ev, "ok"), make_int(static_cast<std::int64_t>(worldline))});
            }


            if (op == "grant") {
                std::int64_t bits = 0, channels = 0, loans = 0, expires = 0, worldline = 0;
                if (a.size() < 7 || !bus_int(a[2], &bits) || !bus_int(a[3], &channels) ||
                    !bus_int(a[4], &loans) || !bus_int(a[5], &expires) ||
                    !bus_int(a[6], &worldline))
                    return bus_error(ev, "validation-failed",
                                     "grant needs bits, channels, loans, expires, worldline");
                aura::bus::GrantBody request{};
                request.caps.bits = static_cast<std::uint64_t>(bits);
                request.caps.channels = static_cast<std::uint64_t>(channels);
                request.caps.max_loans = static_cast<std::uint64_t>(loans);
                request.caps.expires_gen = static_cast<std::uint64_t>(expires);
                request.caps.worldline = static_cast<std::uint64_t>(worldline);
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
                std::int64_t worldline = 0;
                if (a.size() >= 3 && !bus_int(a[2], &worldline))
                    return bus_error(ev, "validation-failed", "audit worldline must be an integer");
                aura::bus::AuditQueryBody req{};
                req.worldline = static_cast<std::uint64_t>(worldline);
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
                             make_int(static_cast<std::int64_t>(w.worldline)),
                             make_int(static_cast<std::int64_t>(w.result)),
                             make_int(static_cast<std::int64_t>(w.version)),
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


            if (op == "project-nodes") {
                auto* flat = active_workspace(ev, *state);
                if (flat == nullptr)
                    return bus_error(ev, "unavailable", "no active workspace AST");
                const std::size_t count = std::min<std::size_t>(flat->size(), 4096);
                if (count == 0)
                    return bus_list(ev, {bus_string(ev, "ok"), make_int(0), make_int(0)});
                std::vector<aura::bus::NodeRecord> records;
                records.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto view = flat->get(static_cast<aura::ast::NodeId>(i));
                    aura::bus::NodeRecord rec{};
                    rec.kind = static_cast<std::uint32_t>(view.tag);
                    rec.slot = static_cast<std::uint64_t>(view.id);
                    rec.parent = aura::bus::kImageNoIndex;
                    rec.first_child = view.children.empty()
                                          ? aura::bus::kImageNoIndex
                                          : static_cast<std::uint64_t>(view.children[0]);
                    rec.next_sibling = aura::bus::kImageNoIndex;
                    rec.sym = view.sym_id == aura::ast::INVALID_SYM
                                  ? aura::bus::kImageNoIndex
                                  : static_cast<std::uint64_t>(view.sym_id);
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
                                             make_int(static_cast<std::int64_t>(recorded.count)),
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
                    return bus_list(ev, {bus_string(ev, "ok"),
                                         make_int(static_cast<std::int64_t>(live.size())),
                                         make_int(1)});
                }
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
