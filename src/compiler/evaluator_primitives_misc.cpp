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

} // namespace

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
