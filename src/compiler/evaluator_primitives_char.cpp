// evaluator_primitives_char.cpp — P0 step 30: char? / string->list / read-line primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "primitives_detail.h"
#include "runtime_shared.h"
#include "security_side_effect.hh" // kEffectMutate (#4503 f64vector-set!)
#include "messaging_bridge.h"
#include "utf8_scalar.hh"        // Issue #4472
#include "compiler/value_tags.h" // Issue #4556: ffi closure ids (c-handle finalizer)

#include <poll.h>
#include <termios.h>
#include <unistd.h>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;
import aura.core.lifetime_pin; // Issue #4556: note_ffi_opaque_create_exempt (#3291)

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

    // Issue #4325: one byte, including from a terminal, without waiting for
    // newline. Pipes and files stay a plain istream read. A tty drops ICANON
    // for this read only, then restores the previous attributes.
    int read_stdin_byte() {
        if (!isatty(STDIN_FILENO))
            return std::cin.get();
        termios saved{};
        if (tcgetattr(STDIN_FILENO, &saved) != 0)
            return std::cin.get();
        termios one = saved;
        one.c_lflag &= ~static_cast<tcflag_t>(ICANON);
        one.c_cc[VMIN] = 1;
        one.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &one) != 0)
            return std::cin.get();
        const int c = std::cin.get();
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return c;
    }

} // namespace

void register_char_primitives(PrimRegistrar add, Evaluator& ev) {

    // Issue #4517: stdin-facing prims (read-line / read-byte / char-ready?)
    // are host io-read channels. Same deny_io shape as register_file_primitives
    // (read, #4504) and register_network_primitives (getenv): allow when the
    // sandbox is off or the principal holds the named cap / io / *; otherwise
    // bump_capability_denial() and return the error with stdin unconsumed and
    // unpolled. No new cap name / effect bit / query key.
    const auto deny_io = [&ev](std::string_view cap, std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(cap) ||
            ev.has_capability(aura::compiler::security::kCapIo) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };

    // Issue #4472: not a blanket is_int. Characters stay fixnums (#4129),
    // so a Unicode scalar — including U+0005 — is a character. Negatives,
    // values above U+10FFFF, surrogates, and non-integers are not.
    add("char?", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        return make_bool(unicode_scalar(as_int(a[0])));
    });

    add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("string->list", [&ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            // Issue #4505: this registration runs after the pair one and
            // wins name lookup, so the reader gate lives here too — no
            // foreign code points are copied into new pairs.
            if (idx < ev.string_heap_.size() &&
                !ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "string->list")) {
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "string->list: cross-tenant string slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            }
            if (idx < ev.string_heap_.size())
                s = ev.string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        // Issue #4472: code points, so list->string can encode them back.
        // This registration runs after the pair one and wins name lookup.
        std::vector<std::int64_t> cps;
        cps.reserve(s.size());
        std::size_t bi = 0;
        Utf8Unit unit;
        while (utf8_unit_at(s, bi, unit)) {
            cps.push_back(static_cast<std::int64_t>(unit.cp));
            bi += unit.nbytes;
        }
        EvalValue result = make_void();
        for (auto it = cps.rbegin(); it != cps.rend(); ++it) {
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_int(*it), result});
            // Issue #4656: every pair mint on the production face carries an
            // owner stamp, so a foreign tenant cannot read this fresh list.
            ev.note_inrange_pair_mint(pid);
            result = make_pair(pid);
        }
        return result;
    });

    add("list->string", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || (!is_pair(a[0]) && !is_void(a[0])))
            return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= ev.pairs_.size())
                break;
            // Issue #4518: kEffectNone inference means dispatch never choked
            // a foreign list here. Compare the mint stamp BEFORE any car is
            // copied into the UTF-8 accumulator; the helper records the
            // IsolationDeny, Soft/Off / record 0 / short stamps stay allow,
            // and the source list is unchanged. Same reader contract as
            // string->list (#4505).
            if (!ev.inrange_slot_tenant_allows(idx, /*vector_heap=*/false,
                                               /*required_effects=*/0, "list->string"))
                return make_primitive_error(
                    ev.string_heap_, ev.error_values_,
                    "list->string: cross-tenant pair slot read denied (#4505)",
                    ev.primitive_error_counter_ptr());
            auto car = ev.pairs_[idx].car;
            if (is_int(car))
                utf8_append(result, as_int(car)); // Issue #4472
            v = ev.pairs_[idx].cdr;
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });

    // Issue #4325: a blank line is "". EOF (or a failed stream) is void,
    // which eof-object? already treats as the eof value. A partial line
    // with no trailing newline is still that line; the next read is eof.
    // Issue #4356: the blocking read runs without the CLI body mutex so a
    // thread fiber can allocate. The string_heap_ push happens only after
    // the lock is back, on this thread's private line buffer.
    add("read-line", [&ev, deny_io](const auto&) {
        // Issue #4517: fail closed behind the io-read fence BEFORE getline.
        // On deny nothing is consumed from stdin; Soft/Off still reads.
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (!std::cin.good())
            return make_void();
        std::string line;
        bool eof = false;
        {
            aura::messaging::DensenessBodyLockWaitGuard unblock_body;
            if (!std::getline(std::cin, line)) {
                if (line.empty())
                    eof = true;
            }
        }
        if (eof)
            return make_void();
        auto id = ev.string_heap_.mint(std::move(line));
        return make_string(id);
    });

    // Issue #4358: non-blocking stdin poll. #t when a later read-line or
    // read-byte would not block (byte already buffered, or the fd is
    // readable, including hangup/EOF). Does not consume the byte.
    // Timeout 0, so the CLI body lock stays held.
    add("char-ready?", [&ev, deny_io](const auto&) {
        // Issue #4517: readability polling is an io-read probe — fence
        // before in_avail/poll so a denied prim neither peeks nor polls.
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (auto* buf = std::cin.rdbuf()) {
            const auto avail = buf->in_avail();
            if (avail > 0)
                return make_bool(true);
        }
        struct pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 0);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            return make_bool(true);
        return make_bool(false);
    });

    // Issue #4325: one byte from stdin. Does not require a newline.
    // EOF is void, distinct from byte 0 and from read-line's "".
    // Issue #4356: same body-mutex drop as read-line across the read.
    add("read-byte", [&ev, deny_io](const auto&) {
        // Issue #4517: fail closed behind the io-read fence BEFORE the byte
        // read. On deny nothing is consumed from stdin; Soft/Off still reads.
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (!std::cin.good())
            return make_void();
        int c = std::char_traits<char>::eof();
        {
            aura::messaging::DensenessBodyLockWaitGuard unblock_body;
            c = read_stdin_byte();
        }
        if (c == std::char_traits<char>::eof())
            return make_void();
        return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(c)));
    });

    // Issue #4055: native encoding prims. The EDSL folds in
    // lib/std/encoding.aura appended each chunk to an immutable accumulator
    // string — O(n²) copy where every intermediate became a string_heap_
    // entry; a 64 KB http-post response under Soft --serve-async denseness
    // retained ~42.8 GB RSS and wedged/OOM-killed Soft mid-batch (third
    // concurrent MiniMax batch → serve_session_timeout). These allocate ONE
    // heap entry per call; lib/std/encoding.aura delegates to them.
    // Issue #4500: `%` names, not `encoding:*` — a prefixed
    // (require "std/encoding") binds encoding:hex-encode to the wrapper itself.
    add("%base64-encode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        // Issue #4518: the % names infer kEffectNone (#4500), so dispatch
        // never choked a foreign string ref. Compare the intern stamp BEFORE
        // any string_heap_ byte is encoded; the helper records the
        // IsolationDeny, Soft/Off / record 0 / short stamps stay allow.
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "%base64-encode"))
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "%base64-encode: cross-tenant string slot read denied (#4505)",
                ev.primitive_error_counter_ptr());
        static const char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const std::string& data = ev.string_heap_[idx];
        std::string out;
        out.reserve((data.size() + 2) / 3 * 4);
        std::size_t i = 0;
        while (i + 2 < data.size()) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            const auto b1 = static_cast<unsigned char>(data[i + 1]);
            const auto b2 = static_cast<unsigned char>(data[i + 2]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            out.push_back(kAlphabet[((b1 & 0xF) << 2) | (b2 >> 6)]);
            out.push_back(kAlphabet[b2 & 0x3F]);
            i += 3;
        }
        const std::size_t rem = data.size() - i;
        if (rem == 1) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[(b0 & 0x3) << 4]);
            out.push_back('=');
            out.push_back('=');
        } else if (rem == 2) {
            const auto b0 = static_cast<unsigned char>(data[i]);
            const auto b1 = static_cast<unsigned char>(data[i + 1]);
            out.push_back(kAlphabet[b0 >> 2]);
            out.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            out.push_back(kAlphabet[(b1 & 0xF) << 2]);
            out.push_back('=');
        }
        auto sid = ev.string_heap_.mint(std::move(out));
        return make_string(sid);
    });

    add("%base64-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        // Issue #4518: same reader gate as %base64-encode — fail closed
        // before any foreign byte is decoded into the new string.
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "%base64-decode"))
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                "%base64-decode: cross-tenant string slot read denied (#4505)",
                ev.primitive_error_counter_ptr());
        const std::string& in = ev.string_heap_[idx];
        auto b64val = [](unsigned char c) -> int {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1;
        };
        std::string out;
        out.reserve(in.size() / 4 * 3);
        unsigned int acc = 0;
        int bits = 0;
        for (unsigned char c : in) {
            if (c == '=')
                break;
            const int v = b64val(c);
            if (v < 0)
                break;
            acc = (acc << 6) | static_cast<unsigned int>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((acc >> bits) & 0xFF));
            }
        }
        auto sid = ev.string_heap_.mint(std::move(out));
        return make_string(sid);
    });

    add("%hex-encode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        // Issue #4518: same reader gate as the base64 pair — fail closed
        // before any foreign byte is hex-encoded (#4500: % names stay
        // kEffectNone, so the stamp compare lives in the body).
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "%hex-encode"))
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "%hex-encode: cross-tenant string slot read denied (#4505)",
                                        ev.primitive_error_counter_ptr());
        static const char kHex[] = "0123456789abcdef";
        const std::string& data = ev.string_heap_[idx];
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data) {
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
        auto sid = ev.string_heap_.mint(std::move(out));
        return make_string(sid);
    });

    add("%hex-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        // Issue #4518: same reader gate — fail closed before any foreign
        // byte is hex-decoded into the new string.
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "%hex-decode"))
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "%hex-decode: cross-tenant string slot read denied (#4505)",
                                        ev.primitive_error_counter_ptr());
        const std::string& in = ev.string_heap_[idx];
        auto hexval = [](unsigned char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(in.size() / 2);
        int hi = -1;
        for (unsigned char c : in) {
            const int v = hexval(c);
            if (v < 0)
                break;
            if (hi < 0) {
                hi = v;
            } else {
                out.push_back(static_cast<char>((hi << 4) | v));
                hi = -1;
            }
        }
        auto sid = ev.string_heap_.mint(std::move(out));
        return make_string(sid);
    });

    add("eof-object?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // EOF is void. A blank line is "" (Issue #4325), so eof-object?
        // is false for that string.
        return make_bool(is_void(a[0]));
    });

    // ── Issue #4503: f64vector — contiguous little-endian doubles ──────────
    // An f64vector is a string slot holding 8*n raw bytes, so elements are
    // machine doubles (no per-element boxing) and existing string GC,
    // tenant and quota plumbing applies unchanged. Bulk load: read the
    // file with read-file (8-bit clean, sandbox-checked) and convert with
    // bytes->f64vector / f32-bytes->f64vector in one pass. f64vector-dot
    // runs a whole multiply-accumulate loop natively (one boxed result).
    auto f64err = [&ev](std::string msg) -> EvalValue {
        return make_primitive_error(ev.string_heap_, ev.error_values_, std::move(msg),
                                    ev.primitive_error_counter_ptr());
    };
    // Issue #4509: an f64vector slot lives in string_heap_, and any fiber
    // can grow that heap (fiber:spawn shares the Evaluator). A concurrent
    // push_back reallocates the pmr::vector and frees the old std::string
    // buffer, so no raw std::string* may leave alloc_storage_lock_ —
    // validate here under the lock and copy/scan the bytes under the lock
    // at the call site, exactly like copy_string_heap_at /
    // string_heap_byte_at. Returns the slot's byte length, or -1 when v
    // is not an f64vector.
    auto f64slot = [&ev](const EvalValue& v) -> std::int64_t {
        if (!is_string(v))
            return -1;
        const auto idx = static_cast<std::size_t>(types::as_string_idx(v));
        std::lock_guard lock(ev.alloc_storage_lock_);
        if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0)
            return -1;
        return static_cast<std::int64_t>(ev.string_heap_[idx].size());
    };
    // Issue #4597: 1 = live 'f64 view (data/n set), 0 = not an f64 view,
    // -1 = a view whose owner was freed or closed. Caller holds
    // alloc_storage_lock_. A non-f64 view and a non-view opaque are 0
    // ("expected an f64vector"), not a view error.
    auto f64_view_locked = [&ev](const EvalValue& v, const double*& data, std::int64_t& n) -> int {
        if (!is_opaque(v))
            return 0;
        const auto packed = types::as_opaque_idx(v);
        if (!ev.ffi_runtime_.is_native_view(packed))
            return 0;
        char* base = nullptr;
        FFIRuntime::NativeView meta;
        if (!ev.ffi_runtime_.resolve_native_view(ev.opaque_heap_, packed, base, meta))
            return -1;
        if (meta.kind != FFIRuntime::ViewKind::F64)
            return 0;
        data = reinterpret_cast<const double*>(base);
        n = static_cast<std::int64_t>(meta.count);
        return 1;
    };
    auto as_real = [](const EvalValue& v, double& out) {
        if (is_float(v)) {
            out = as_float(v);
            return true;
        }
        if (is_int(v)) {
            out = static_cast<double>(as_int(v));
            return true;
        }
        return false;
    };
    auto new_f64 = [&ev](std::string bytes) -> EvalValue {
        // Issue #4509: mint through push_string_heap — size + push_back +
        // index must be atomic under alloc_storage_lock_ (fiber:spawn
        // shares the Evaluator), and the slot needs the in-range tenant
        // stamp (#4428) the unlocked push_back skipped. Same call as every
        // other string mint; not a second pin registry.
        return make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::move(bytes))));
    };
    auto rd = [](const std::string& s, std::size_t i) {
        double d;
        std::memcpy(&d, s.data() + i * 8, 8);
        return d;
    };

    add("make-f64vector", [f64err, as_real, new_f64](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || as_int(a[0]) < 0 || as_int(a[0]) > (1LL << 31))
            return f64err("make-f64vector: expected a length in [0, 2^31]");
        double fill = 0.0;
        if (a.size() > 1 && !as_real(a[1], fill))
            return f64err("make-f64vector: fill must be a number");
        const auto n = static_cast<std::size_t>(as_int(a[0]));
        std::string bytes(n * 8, '\0');
        for (std::size_t i = 0; i < n; ++i)
            std::memcpy(bytes.data() + i * 8, &fill, 8);
        return new_f64(std::move(bytes));
    });
    add("f64vector-length",
        [f64err, &ev, f64slot, f64_view_locked](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty())
                return f64err("f64vector-length: expected an f64vector");
            // Issue #4597: a live 'f64 view. Branch before as_string_idx. Do not
            // tenant-check a view (it is not a string slot).
            if (is_opaque(a[0])) {
                std::lock_guard lock(ev.alloc_storage_lock_);
                const double* data = nullptr;
                std::int64_t n = 0;
                const int rc = f64_view_locked(a[0], data, n);
                if (rc < 0)
                    return f64err("f64vector-length: the view's native memory was freed or closed");
                if (rc == 0)
                    return f64err("f64vector-length: expected an f64vector");
                return make_int(n);
            }
            if (!is_string(a[0]))
                return f64err("f64vector-length: expected an f64vector");
            // Issue #4518: the slot size is payload metadata (an f64vector is a
            // string slot), so compare the intern stamp before f64slot reads it.
            // kEffectNone inference means dispatch never choked a foreign ref;
            // the helper records the IsolationDeny and record 0 / short stamps /
            // Soft-Off stay allow. f64vector-set! keeps its own Mutate gate.
            const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
            if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "f64vector-length"))
                return f64err("f64vector-length: cross-tenant string slot read denied (#4505)");
            const auto bytes = f64slot(a[0]);
            if (bytes < 0)
                return f64err("f64vector-length: expected an f64vector");
            return make_int(bytes / 8);
        });
    add("f64vector-ref",
        [f64err, &ev, rd, f64_view_locked](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 2 || !is_int(a[1]) || !(is_string(a[0]) || is_opaque(a[0])))
                return f64err("f64vector-ref: expected (f64vector index)");
            const auto i = as_int(a[1]);
            // Issue #4509: copy the element out under alloc_storage_lock_ — the
            // heap can reallocate between validation and read on another fiber.
            // Issue #4597: an 'f64 view is read under the same lock. Not a string
            // slot, so no tenant check.
            std::lock_guard lock(ev.alloc_storage_lock_);
            if (is_opaque(a[0])) {
                const double* data = nullptr;
                std::int64_t n = 0;
                const int rc = f64_view_locked(a[0], data, n);
                if (rc < 0)
                    return f64err("f64vector-ref: the view's native memory was freed or closed");
                if (rc == 0)
                    return f64err("f64vector-ref: expected (f64vector index)");
                if (i < 0 || i >= n)
                    return f64err(
                        std::format("f64vector-ref: index {} out of range for length {}", i, n));
                return make_float(data[static_cast<std::size_t>(i)]);
            }
            const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
            if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0)
                return f64err("f64vector-ref: expected (f64vector index)");
            // Issue #4518: the returned double is the payload — compare the
            // intern stamp under the storage lock (same shape as the #4428
            // f64vector-set! gate) before any byte is copied out.
            if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "f64vector-ref"))
                return f64err("f64vector-ref: cross-tenant string slot read denied (#4505)");
            const auto& s = ev.string_heap_[idx];
            const auto n = static_cast<std::int64_t>(s.size() / 8);
            if (i < 0 || i >= n)
                return f64err(
                    std::format("f64vector-ref: index {} out of range for length {}", i, n));
            return make_float(rd(s, static_cast<std::size_t>(i)));
        });
    add("f64vector-set!",
        [&ev, f64err, f64slot, as_real](std::span<const EvalValue> a) -> EvalValue {
            double x = 0.0;
            if (a.size() < 3 || f64slot(a[0]) < 0 || !is_int(a[1]) || !as_real(a[2], x))
                return f64err("f64vector-set!: expected (f64vector index number)");
            std::lock_guard lock(ev.alloc_storage_lock_);
            const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
            // Issue #4519: f64slot validated under its own critical section
            // and released the lock; a concurrent restore_panic_checkpoint
            // shrink can drop the slot before this second critical section
            // re-acquires. Refuse when idx is past the (shrunk) heap — or
            // the slot no longer holds a %8 payload — before operator[];
            // inrange_string_tenant_allows does not check the heap length.
            if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0)
                return f64err("f64vector-set!: expected (f64vector index number)");
            if (!ev.inrange_string_tenant_allows(idx, aura::compiler::security::kEffectMutate,
                                                 "f64vector-set!"))
                return f64err("f64vector-set!: cross-tenant slot write denied (#4428)");
            auto& s = ev.string_heap_[idx];
            const auto i = as_int(a[1]);
            const auto n = static_cast<std::int64_t>(s.size() / 8);
            if (i < 0 || i >= n)
                return f64err(
                    std::format("f64vector-set!: index {} out of range for length {}", i, n));
            std::memcpy(s.data() + static_cast<std::size_t>(i) * 8, &x, 8);
            ev.string_heap_.note_write(idx); // Issue #4548: in-place bytes
            return make_void();
        });
    // (f64vector-dot a b [a-off b-off n]) — sum of a[a-off+k]*b[b-off+k].
    add("f64vector-dot", [f64err, &ev, f64_view_locked](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2)
            return f64err("f64vector-dot: expected two f64vectors");
        // Issue #4509: validate both sides and run the MAC scan under
        // alloc_storage_lock_ — the loop previously held &string_heap_[idx]
        // for the whole dot while a concurrent mint (push_string_heap) could
        // reallocate the heap and free the buffer under it. The pointers
        // stay inside the locked region; nothing is published.
        // Issue #4597: either side may be a live 'f64 view. A view is not a
        // string slot, so it is not tenant-checked. Both pointers stay valid
        // for the whole MAC because the lock is held.
        std::lock_guard lock(ev.alloc_storage_lock_);
        auto load = [&](const EvalValue& v, const double*& p, std::int64_t& n) -> std::string {
            if (is_opaque(v)) {
                const int rc = f64_view_locked(v, p, n);
                if (rc < 0)
                    return "f64vector-dot: the view's native memory was freed or closed";
                if (rc == 0)
                    return "f64vector-dot: expected two f64vectors";
                return {};
            }
            if (!is_string(v))
                return "f64vector-dot: expected two f64vectors";
            const auto idx = static_cast<std::size_t>(types::as_string_idx(v));
            if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0)
                return "f64vector-dot: expected two f64vectors";
            // Issue #4518: the MAC result leaks element values, so a string
            // slot needs the intern-stamp compare before any byte is scanned.
            if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "f64vector-dot"))
                return "f64vector-dot: cross-tenant string slot read denied (#4505)";
            p = reinterpret_cast<const double*>(ev.string_heap_[idx].data());
            n = static_cast<std::int64_t>(ev.string_heap_[idx].size() / 8);
            return {};
        };
        const double* xp = nullptr;
        const double* yp = nullptr;
        std::int64_t nx = 0, ny = 0;
        if (auto e = load(a[0], xp, nx); !e.empty())
            return f64err(std::move(e));
        if (auto e = load(a[1], yp, ny); !e.empty())
            return f64err(std::move(e));
        std::int64_t xo = 0, yo = 0, n = std::min(nx, ny);
        if (a.size() >= 5) {
            if (!is_int(a[2]) || !is_int(a[3]) || !is_int(a[4]))
                return f64err("f64vector-dot: offsets and count must be integers");
            xo = as_int(a[2]);
            yo = as_int(a[3]);
            n = as_int(a[4]);
        }
        if (xo < 0 || yo < 0 || n < 0 || xo + n > nx || yo + n > ny)
            return f64err("f64vector-dot: range out of bounds");
        double acc = 0.0;
        for (std::int64_t k = 0; k < n; ++k)
            acc += xp[static_cast<std::size_t>(xo + k)] * yp[static_cast<std::size_t>(yo + k)];
        return make_float(acc);
    });
    // (bytes->f64vector str [byte-offset count]) — little-endian float64.
    // (f32-bytes->f64vector str [byte-offset count]) — float32, widened.
    auto from_bytes = [f64err, new_f64](std::span<const EvalValue> a, std::string& src,
                                        std::size_t width, const char* who) -> EvalValue {
        std::int64_t off = 0;
        std::int64_t cnt = static_cast<std::int64_t>(src.size() / width);
        if (a.size() >= 2) {
            if (!is_int(a[1]) || as_int(a[1]) < 0 ||
                static_cast<std::size_t>(as_int(a[1])) > src.size())
                return f64err(std::format("{}: bad byte offset", who));
            off = as_int(a[1]);
            cnt = static_cast<std::int64_t>((src.size() - static_cast<std::size_t>(off)) / width);
        }
        if (a.size() >= 3) {
            if (!is_int(a[2]) || as_int(a[2]) < 0 || as_int(a[2]) > cnt)
                return f64err(std::format("{}: count exceeds the available bytes", who));
            cnt = as_int(a[2]);
        }
        std::string out(static_cast<std::size_t>(cnt) * 8, '\0');
        for (std::int64_t k = 0; k < cnt; ++k) {
            const char* p = src.data() + off + k * static_cast<std::int64_t>(width);
            double d;
            if (width == 8) {
                std::memcpy(&d, p, 8);
            } else {
                float f;
                std::memcpy(&f, p, 4);
                d = static_cast<double>(f);
            }
            std::memcpy(out.data() + k * 8, &d, 8);
        }
        return new_f64(std::move(out));
    };
    add("bytes->f64vector", [&ev, f64err, from_bytes](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return f64err("bytes->f64vector: expected a byte string");
        // Issue #4509: snapshot the source bytes under alloc_storage_lock_
        // (copy_string_heap_at protocol) — the unlocked heap read raced a
        // concurrent push_back realloc between the bounds check and copy.
        std::string src;
        {
            std::lock_guard lock(ev.alloc_storage_lock_);
            const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
            if (idx >= ev.string_heap_.size())
                return f64err("bytes->f64vector: expected a byte string");
            // Issue #4518: the snapshot IS the payload copy — stamp compare
            // first, exactly like the f64vector-set! write gate (#4428).
            if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "bytes->f64vector"))
                return f64err("bytes->f64vector: cross-tenant string slot read denied (#4505)");
            src = ev.string_heap_[idx];
        }
        return from_bytes(a, src, 8, "bytes->f64vector");
    });
    add("f32-bytes->f64vector",
        [&ev, f64err, from_bytes](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_string(a[0]))
                return f64err("f32-bytes->f64vector: expected a byte string");
            // Issue #4509: same locked snapshot as bytes->f64vector.
            std::string src;
            {
                std::lock_guard lock(ev.alloc_storage_lock_);
                const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
                if (idx >= ev.string_heap_.size())
                    return f64err("f32-bytes->f64vector: expected a byte string");
                // Issue #4518: same snapshot gate as bytes->f64vector.
                if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0,
                                                     "f32-bytes->f64vector"))
                    return f64err(
                        "f32-bytes->f64vector: cross-tenant string slot read denied (#4505)");
                src = ev.string_heap_[idx];
            }
            return from_bytes(a, src, 4, "f32-bytes->f64vector");
        });
    // (bytes-i32-ref str byte-offset) — little-endian int32 (checkpoint headers).
    add("bytes-i32-ref", [&ev, f64err](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return f64err("bytes-i32-ref: expected (string byte-offset)");
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return f64err("bytes-i32-ref: expected (string byte-offset)");
        // Issue #4518: the four copied bytes are the payload — compare the
        // intern stamp before any byte is copied out.
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, "bytes-i32-ref"))
            return f64err("bytes-i32-ref: cross-tenant string slot read denied (#4505)");
        const auto& s = ev.string_heap_[idx];
        const auto off = as_int(a[1]);
        if (off < 0 || static_cast<std::size_t>(off) + 4 > s.size())
            return f64err("bytes-i32-ref: offset out of range");
        std::int32_t v;
        std::memcpy(&v, s.data() + off, 4);
        return make_int(v);
    });

    // ── Issue #4556: native memory views, bulk f64vector ops, handles ──────
    // A view (c-view) is a typed window (f64 / i64 / u64 / i32 / u32 / u8)
    // onto native memory a C library owns (c-alloc / c-opaque /
    // c-struct-ref pointer). Element reads are a native load each, with no
    // byte-string snapshot per poll; c-view-copy! / c-view->f64vector move
    // a whole window into an f64vector in one native loop; and
    // c-view-read-consistent! copies a window under a seqlock word so a
    // concurrently written block is never read torn. Every access re-reads
    // the owner's opaque slot: after c-free / c-close! of the owner the view
    // raises a catchable error instead of reading freed memory. Views and
    // handles are c-* names: installed by (require "std/ffi") with the rest
    // of the FFI capability (kEffectFfi under sandbox).
    using ViewKind = FFIRuntime::ViewKind;
    using NativeView = FFIRuntime::NativeView;
    auto view_kind_of = [&ev](const EvalValue& v, ViewKind& out) {
        if (!is_string(v))
            return false;
        const std::string k = ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(v)));
        if (k == "f64" || k == "double")
            out = ViewKind::F64;
        else if (k == "i64")
            out = ViewKind::I64;
        else if (k == "u64")
            out = ViewKind::U64;
        else if (k == "i32")
            out = ViewKind::I32;
        else if (k == "u32")
            out = ViewKind::U32;
        else if (k == "u8")
            out = ViewKind::U8;
        else
            return false;
        return true;
    };
    // Element load / store. p is base + i * width (may be unaligned).
    auto view_load = [](const char* p, ViewKind k) -> EvalValue {
        switch (k) {
            case ViewKind::F64: {
                double d;
                std::memcpy(&d, p, 8);
                return make_float(d);
            }
            case ViewKind::I64: {
                std::int64_t x;
                std::memcpy(&x, p, 8);
                return types::make_int_or_promote(x);
            }
            case ViewKind::U64: {
                std::uint64_t x;
                std::memcpy(&x, p, 8);
                if (x > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return make_float(static_cast<double>(x));
                return types::make_int_or_promote(static_cast<std::int64_t>(x));
            }
            case ViewKind::I32: {
                std::int32_t x;
                std::memcpy(&x, p, 4);
                return make_int(x);
            }
            case ViewKind::U32: {
                std::uint32_t x;
                std::memcpy(&x, p, 4);
                return make_int(static_cast<std::int64_t>(x));
            }
            case ViewKind::U8:
                return make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*p)));
        }
        return make_void();
    };
    auto view_load_f64 = [](const char* p, ViewKind k) -> double {
        switch (k) {
            case ViewKind::F64: {
                double d;
                std::memcpy(&d, p, 8);
                return d;
            }
            case ViewKind::I64: {
                std::int64_t x;
                std::memcpy(&x, p, 8);
                return static_cast<double>(x);
            }
            case ViewKind::U64: {
                std::uint64_t x;
                std::memcpy(&x, p, 8);
                return static_cast<double>(x);
            }
            case ViewKind::I32: {
                std::int32_t x;
                std::memcpy(&x, p, 4);
                return static_cast<double>(x);
            }
            case ViewKind::U32: {
                std::uint32_t x;
                std::memcpy(&x, p, 4);
                return static_cast<double>(x);
            }
            case ViewKind::U8:
                return static_cast<double>(static_cast<unsigned char>(*p));
        }
        return 0.0;
    };
    // Resolve a view argument under alloc_storage_lock_ (caller holds it).
    // Returns an error message, or empty on success.
    auto view_at = [&ev](const EvalValue& v, const char* who, char*& base,
                         NativeView& meta) -> std::string {
        if (!is_opaque(v))
            return std::format("{}: expected a native view (c-view)", who);
        const auto packed = types::as_opaque_idx(v);
        if (!ev.ffi_runtime_.is_native_view(packed))
            return std::format("{}: expected a native view (c-view)", who);
        if (!ev.ffi_runtime_.resolve_native_view(ev.opaque_heap_, packed, base, meta))
            return std::format("{}: the view's native memory was freed or closed", who);
        return {};
    };

    // (c-view ptr byte-offset count kind [byte-length]) → view.
    // ptr is an opaque (c-alloc / c-opaque / c-struct-ref pointer field) or
    // a view (offsets compose; the new view must fit inside it). The window
    // must fit in the c-alloc size, the optional byte-length, or the parent
    // view; a raw pointer with no known size requires byte-length.
    ev.defer_std_host_prim("c-view", [&ev, f64err, view_kind_of](const auto& a) -> EvalValue {
        if (a.size() < 4 || !is_opaque(a[0]) || !is_int(a[1]) || !is_int(a[2]))
            return f64err("c-view: expected (c-view ptr byte-offset count kind [byte-length])");
        ViewKind kind{};
        if (!view_kind_of(a[3], kind))
            return f64err("c-view: kind must be one of f64 i64 u64 i32 u32 u8");
        const auto off = as_int(a[1]);
        const auto cnt = as_int(a[2]);
        if (off < 0 || cnt < 0 || cnt > (1LL << 40))
            return f64err("c-view: byte-offset and count must be non-negative");
        const auto width = FFIRuntime::view_width(kind);
        std::lock_guard lock(ev.alloc_storage_lock_);
        auto& rt = ev.ffi_runtime_;
        auto& oh = ev.opaque_heap_;
        const auto src_packed = types::as_opaque_idx(a[0]);
        std::uint32_t src = 0;
        if (!rt.locate_opaque(oh, src_packed, src) || oh[src] == nullptr)
            return f64err("c-view: pointer is null, freed or closed");
        std::size_t owner = src;
        std::uint32_t owner_gen = rt.opaque_generation_at(src);
        std::size_t base_off = 0;
        std::int64_t limit = -1; // bytes available from the owner base + base_off
        if (rt.is_native_view(src_packed)) {
            char* vb = nullptr;
            NativeView pv;
            if (!rt.resolve_native_view(oh, src_packed, vb, pv))
                return f64err("c-view: the parent view's native memory was freed or closed");
            // A view of a view names the root owner, not the parent view slot,
            // so c-free of the parent view leaves the sub-view usable.
            owner = pv.owner;
            owner_gen = pv.owner_gen;
            base_off = pv.offset;
            limit = static_cast<std::int64_t>(pv.count * FFIRuntime::view_width(pv.kind));
        } else if (auto known = rt.opaque_size_of(oh[src]); known != 0) {
            limit = static_cast<std::int64_t>(known);
        }
        if (a.size() >= 5) {
            if (!is_int(a[4]) || as_int(a[4]) < 0)
                return f64err("c-view: byte-length must be a non-negative integer");
            if (limit < 0 || as_int(a[4]) < limit)
                limit = as_int(a[4]);
        }
        if (limit < 0)
            return f64err("c-view: pointer has no known size; pass byte-length");
        // cnt <= 2^40 and width <= 8, so cnt * width cannot overflow; check
        // off first so limit - off cannot underflow.
        if (off > limit || cnt * static_cast<std::int64_t>(width) > limit - off)
            return f64err(std::format("c-view: window of {} x {}-byte elements at offset {} "
                                      "exceeds {} bytes",
                                      cnt, width, off, limit));
        const std::size_t voff = base_off + static_cast<std::size_t>(off);
        // Copy the native address before mint: a fresh append can reallocate
        // oh, and oh[owner] is the native pointer, not an element address.
        void* view_ptr = static_cast<char*>(oh[owner]) + voff;
        auto vpacked = rt.mint_opaque_slot(oh, view_ptr);
        if (vpacked == ~std::uint64_t{0}) {
            const auto slot = static_cast<std::uint32_t>(oh.size());
            oh.push_back(view_ptr);
            rt.note_appended_opaque(oh);
            vpacked = types::pack_opaque_index(slot, 0);
        }
        // Issue #3022 / #3291: GENERAL_OBJECT_PIN_EXEMPT: external-native-addr
        // A view addresses native memory the C side (or c-alloc) owns, never
        // an arena object; every access re-derives the address from the
        // owner's slot, so this element is never the address that is read.
        aura::core::lifetime::note_ffi_opaque_create_exempt("external-native-addr");
        rt.add_native_view(types::opaque_slot_index(vpacked),
                           NativeView{owner, voff, static_cast<std::size_t>(cnt), kind, owner_gen});
        return types::make_opaque(vpacked);
    });
    ev.defer_std_host_prim("c-view?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_opaque(a[0]))
            return make_bool(false);
        std::lock_guard lock(ev.alloc_storage_lock_);
        return make_bool(ev.ffi_runtime_.is_native_view(types::as_opaque_idx(a[0])));
    });
    ev.defer_std_host_prim("c-view-length", [&ev, f64err, view_at](const auto& a) -> EvalValue {
        if (a.empty())
            return f64err("c-view-length: expected a native view (c-view)");
        std::lock_guard lock(ev.alloc_storage_lock_);
        char* base = nullptr;
        NativeView m;
        if (auto e = view_at(a[0], "c-view-length", base, m); !e.empty())
            return f64err(std::move(e));
        return make_int(static_cast<std::int64_t>(m.count));
    });
    ev.defer_std_host_prim(
        "c-view-ref", [&ev, f64err, view_at, view_load](const auto& a) -> EvalValue {
            if (a.size() < 2 || !is_int(a[1]))
                return f64err("c-view-ref: expected (view index)");
            std::lock_guard lock(ev.alloc_storage_lock_);
            char* base = nullptr;
            NativeView m;
            if (auto e = view_at(a[0], "c-view-ref", base, m); !e.empty())
                return f64err(std::move(e));
            const auto i = as_int(a[1]);
            if (i < 0 || static_cast<std::uint64_t>(i) >= m.count)
                return f64err(
                    std::format("c-view-ref: index {} out of range for length {}", i, m.count));
            return view_load(base + static_cast<std::size_t>(i) * FFIRuntime::view_width(m.kind),
                             m.kind);
        });
    ev.defer_std_host_prim("c-view-set!", [&ev, f64err, view_at](const auto& a) -> EvalValue {
        if (a.size() < 3 || !is_int(a[1]) || !(is_int(a[2]) || is_float(a[2])))
            return f64err("c-view-set!: expected (view index number)");
        std::lock_guard lock(ev.alloc_storage_lock_);
        char* base = nullptr;
        NativeView m;
        if (auto e = view_at(a[0], "c-view-set!", base, m); !e.empty())
            return f64err(std::move(e));
        const auto i = as_int(a[1]);
        if (i < 0 || static_cast<std::uint64_t>(i) >= m.count)
            return f64err(
                std::format("c-view-set!: index {} out of range for length {}", i, m.count));
        char* p = base + static_cast<std::size_t>(i) * FFIRuntime::view_width(m.kind);
        const bool fl = is_float(a[2]);
        const double d = fl ? as_float(a[2]) : static_cast<double>(as_int(a[2]));
        const std::int64_t n = fl ? static_cast<std::int64_t>(d) : as_int(a[2]);
        switch (m.kind) {
            case ViewKind::F64:
                std::memcpy(p, &d, 8);
                break;
            case ViewKind::I64:
            case ViewKind::U64:
                std::memcpy(p, &n, 8);
                break;
            case ViewKind::I32:
            case ViewKind::U32: {
                const auto x = static_cast<std::uint32_t>(n);
                std::memcpy(p, &x, 4);
                break;
            }
            case ViewKind::U8:
                *p = static_cast<char>(static_cast<unsigned char>(n));
                break;
        }
        return make_void();
    });
    // Copy view elements (as doubles) into an f64vector under the lock.
    // copy_fn(dst_bytes, src_base, meta, dst_off) does the actual loads.
    auto f64_dst_gate = [&ev](const EvalValue& v, const char* who, std::size_t& idx,
                              std::string& err) -> bool {
        if (!is_string(v)) {
            err = std::format("{}: destination must be an f64vector", who);
            return false;
        }
        idx = static_cast<std::size_t>(types::as_string_idx(v));
        if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0) {
            err = std::format("{}: destination must be an f64vector", who);
            return false;
        }
        if (!ev.inrange_string_tenant_allows(idx, aura::compiler::security::kEffectMutate, who)) {
            err = std::format("{}: cross-tenant slot write denied (#4428)", who);
            return false;
        }
        return true;
    };
    auto copy_view = [view_load_f64](char* dst, const char* base, const NativeView& m) {
        if (m.kind == ViewKind::F64) {
            std::memcpy(dst, base, m.count * 8);
            return;
        }
        const auto w = FFIRuntime::view_width(m.kind);
        for (std::size_t k = 0; k < m.count; ++k) {
            const double d = view_load_f64(base + k * w, m.kind);
            std::memcpy(dst + k * 8, &d, 8);
        }
    };
    // (c-view-copy! view f64vector [dst-index]) → count copied.
    ev.defer_std_host_prim(
        "c-view-copy!",
        [&ev, f64err, view_at, f64_dst_gate, copy_view](const auto& a) -> EvalValue {
            if (a.size() < 2)
                return f64err("c-view-copy!: expected (view f64vector [dst-index])");
            std::lock_guard lock(ev.alloc_storage_lock_);
            char* base = nullptr;
            NativeView m;
            if (auto e = view_at(a[0], "c-view-copy!", base, m); !e.empty())
                return f64err(std::move(e));
            std::size_t idx = 0;
            std::string err;
            if (!f64_dst_gate(a[1], "c-view-copy!", idx, err))
                return f64err(std::move(err));
            std::int64_t dst_off = 0;
            if (a.size() >= 3) {
                if (!is_int(a[2]) || as_int(a[2]) < 0)
                    return f64err("c-view-copy!: dst-index must be a non-negative integer");
                dst_off = as_int(a[2]);
            }
            auto& s = ev.string_heap_[idx];
            const auto n = static_cast<std::int64_t>(s.size() / 8);
            if (dst_off + static_cast<std::int64_t>(m.count) > n)
                return f64err(
                    std::format("c-view-copy!: {} elements at index {} do not fit length {}",
                                m.count, dst_off, n));
            copy_view(s.data() + static_cast<std::size_t>(dst_off) * 8, base, m);
            ev.string_heap_.note_write(idx); // Issue #4548: in-place bytes
            return make_int(static_cast<std::int64_t>(m.count));
        });
    // (c-view->f64vector view) → fresh f64vector with the window as doubles.
    ev.defer_std_host_prim(
        "c-view->f64vector", [&ev, f64err, view_at, copy_view](const auto& a) -> EvalValue {
            if (a.empty())
                return f64err("c-view->f64vector: expected a native view");
            std::lock_guard lock(ev.alloc_storage_lock_);
            char* base = nullptr;
            NativeView m;
            if (auto e = view_at(a[0], "c-view->f64vector", base, m); !e.empty())
                return f64err(std::move(e));
            std::string out(m.count * 8, '\0');
            copy_view(out.data(), base, m);
            return make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::move(out))));
        });
    // (c-view-read-consistent! view f64vector seq-view [seq-index max-tries])
    // Seqlock reader: the C writer bumps a u64 sequence word to odd, writes
    // the block, then bumps it to even. Copy only between two equal even
    // reads; retry otherwise (bounded). seq-view is a u64/i64 view holding
    // the sequence word. Returns the number of retries.
    ev.defer_std_host_prim(
        "c-view-read-consistent!",
        [&ev, f64err, view_at, f64_dst_gate, copy_view](const auto& a) -> EvalValue {
            if (a.size() < 3)
                return f64err(
                    "c-view-read-consistent!: expected (view f64vector seq-view [seq-index "
                    "max-tries])");
            std::int64_t seq_i = 0, max_tries = 1000000;
            if (a.size() >= 4) {
                if (!is_int(a[3]) || as_int(a[3]) < 0)
                    return f64err("c-view-read-consistent!: seq-index must be >= 0");
                seq_i = as_int(a[3]);
            }
            if (a.size() >= 5) {
                if (!is_int(a[4]) || as_int(a[4]) < 1)
                    return f64err("c-view-read-consistent!: max-tries must be >= 1");
                max_tries = as_int(a[4]);
            }
            std::lock_guard lock(ev.alloc_storage_lock_);
            char* base = nullptr;
            NativeView m;
            if (auto e = view_at(a[0], "c-view-read-consistent!", base, m); !e.empty())
                return f64err(std::move(e));
            char* sbase = nullptr;
            NativeView sm;
            if (auto e = view_at(a[2], "c-view-read-consistent!", sbase, sm); !e.empty())
                return f64err(std::move(e));
            if (sm.kind != ViewKind::U64 && sm.kind != ViewKind::I64)
                return f64err("c-view-read-consistent!: seq-view must be a u64 or i64 view");
            if (static_cast<std::uint64_t>(seq_i) >= sm.count)
                return f64err("c-view-read-consistent!: seq-index out of range");
            char* seq_p = sbase + static_cast<std::size_t>(seq_i) * 8;
            if (reinterpret_cast<std::uintptr_t>(seq_p) % alignof(std::uint64_t) != 0)
                return f64err("c-view-read-consistent!: sequence word is not 8-byte aligned");
            std::size_t idx = 0;
            std::string err;
            if (!f64_dst_gate(a[1], "c-view-read-consistent!", idx, err))
                return f64err(std::move(err));
            auto& s = ev.string_heap_[idx];
            if (s.size() / 8 < m.count)
                return f64err(std::format(
                    "c-view-read-consistent!: destination length {} is shorter than the view "
                    "({})",
                    s.size() / 8, m.count));
            std::atomic_ref<std::uint64_t> seq(*reinterpret_cast<std::uint64_t*>(seq_p));
            std::string tmp(m.count * 8, '\0');
            for (std::int64_t tries = 0; tries < max_tries; ++tries) {
                const auto s1 = seq.load(std::memory_order_acquire);
                if ((s1 & 1U) != 0) {
                    std::this_thread::yield();
                    continue;
                }
                copy_view(tmp.data(), base, m);
                std::atomic_thread_fence(std::memory_order_acquire);
                const auto s2 = seq.load(std::memory_order_relaxed);
                if (s1 == s2) {
                    std::memcpy(s.data(), tmp.data(), tmp.size());
                    ev.string_heap_.note_write(idx); // Issue #4548
                    return make_int(tries);
                }
            }
            return f64err(std::format(
                "c-view-read-consistent!: no stable snapshot after {} tries (writer stuck odd?)",
                max_tries));
        });

    // Bulk f64vector ops: whole loops natively, one boxed result.
    // Shared read gate: slot is an f64vector the caller's tenant may read.
    auto f64_src = [&ev](const EvalValue& v, const char* who, std::size_t& idx,
                         std::string& err) -> bool {
        if (!is_string(v)) {
            err = std::format("{}: expected an f64vector", who);
            return false;
        }
        idx = static_cast<std::size_t>(types::as_string_idx(v));
        if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0) {
            err = std::format("{}: expected an f64vector", who);
            return false;
        }
        if (!ev.inrange_string_tenant_allows(idx, /*required_effects=*/0, who)) {
            err = std::format("{}: cross-tenant string slot read denied (#4505)", who);
            return false;
        }
        return true;
    };
    // (f64vector-sum v) → float.
    add("f64vector-sum", [&ev, f64err, f64_src, rd](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return f64err("f64vector-sum: expected an f64vector");
        std::lock_guard lock(ev.alloc_storage_lock_);
        std::size_t idx = 0;
        std::string err;
        if (!f64_src(a[0], "f64vector-sum", idx, err))
            return f64err(std::move(err));
        const auto& s = ev.string_heap_[idx];
        double acc = 0.0;
        for (std::size_t k = 0, n = s.size() / 8; k < n; ++k)
            acc += rd(s, k);
        return make_float(acc);
    });
    // (f64vector-max-index v) → index of the largest element (first on
    // ties; NaN never wins), or -1 when empty / all NaN.
    add("f64vector-max-index",
        [&ev, f64err, f64_src, rd](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty())
                return f64err("f64vector-max-index: expected an f64vector");
            std::lock_guard lock(ev.alloc_storage_lock_);
            std::size_t idx = 0;
            std::string err;
            if (!f64_src(a[0], "f64vector-max-index", idx, err))
                return f64err(std::move(err));
            const auto& s = ev.string_heap_[idx];
            std::int64_t best = -1;
            double bv = 0.0;
            for (std::size_t k = 0, n = s.size() / 8; k < n; ++k) {
                const double d = rd(s, k);
                if (d == d && (best < 0 || d > bv)) {
                    best = static_cast<std::int64_t>(k);
                    bv = d;
                }
            }
            return make_int(best);
        });
    // (f64vector-sub! dst a b) → dst[i] = a[i] - b[i] (all one length).
    // dst may alias a or b (deltas: (f64vector-sub! d cur prev)).
    add("f64vector-sub!",
        [&ev, f64err, f64_src, f64_dst_gate, rd](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 3)
                return f64err("f64vector-sub!: expected (dst a b)");
            std::lock_guard lock(ev.alloc_storage_lock_);
            std::size_t di = 0, xi = 0, yi = 0;
            std::string err;
            if (!f64_dst_gate(a[0], "f64vector-sub!", di, err) ||
                !f64_src(a[1], "f64vector-sub!", xi, err) ||
                !f64_src(a[2], "f64vector-sub!", yi, err))
                return f64err(std::move(err));
            auto& d = ev.string_heap_[di];
            const auto& x = ev.string_heap_[xi];
            const auto& y = ev.string_heap_[yi];
            if (x.size() != d.size() || y.size() != d.size())
                return f64err("f64vector-sub!: lengths differ");
            for (std::size_t k = 0, n = d.size() / 8; k < n; ++k) {
                const double r = rd(x, k) - rd(y, k);
                std::memcpy(d.data() + k * 8, &r, 8);
            }
            ev.string_heap_.note_write(di); // Issue #4548
            return make_void();
        });
    // (f64vector-ewma! acc x alpha) → acc[i] = acc[i] + alpha*(x[i]-acc[i]).
    add("f64vector-ewma!",
        [&ev, f64err, f64_src, f64_dst_gate, rd,
         as_real](std::span<const EvalValue> a) -> EvalValue {
            double alpha = 0.0;
            if (a.size() < 3 || !as_real(a[2], alpha))
                return f64err("f64vector-ewma!: expected (acc x alpha)");
            std::lock_guard lock(ev.alloc_storage_lock_);
            std::size_t di = 0, xi = 0;
            std::string err;
            if (!f64_dst_gate(a[0], "f64vector-ewma!", di, err) ||
                !f64_src(a[1], "f64vector-ewma!", xi, err))
                return f64err(std::move(err));
            auto& d = ev.string_heap_[di];
            const auto& x = ev.string_heap_[xi];
            if (x.size() != d.size())
                return f64err("f64vector-ewma!: lengths differ");
            for (std::size_t k = 0, n = d.size() / 8; k < n; ++k) {
                const double c = rd(d, k);
                const double r = c + alpha * (rd(x, k) - c);
                std::memcpy(d.data() + k * 8, &r, 8);
            }
            ev.string_heap_.note_write(di); // Issue #4548
            return make_void();
        });

    // ── Handles: an opaque with a finalizer that runs exactly once ─────────
    // (c-handle ptr finalizer [tag]) → handle. ptr is an opaque or an int
    // address; finalizer is a c-func procedure taking one Opaque / Int
    // (its return is ignored), or #f for none. (c-close! h) runs the
    // finalizer once and nulls the slot: #t the first time, #f after. c-free
    // on a handle closes it the same way (no libc free of a foreign
    // pointer). Any view onto a closed handle raises. File-mode reclaim and
    // gc-heap / gc-temp run the finalizer once when the handle is unreachable
    // or the heap is reset (Issue #4597). The destructor does not.
    ev.defer_std_host_prim("c-handle", [&ev, f64err](const auto& a) -> EvalValue {
        if (a.size() < 2 || !(is_opaque(a[0]) || is_int(a[0])))
            return f64err("c-handle: expected (c-handle ptr finalizer [tag])");
        void* fin = nullptr;
        if (is_closure(a[1]) && types::is_ffi_closure_id(types::as_closure_id(a[1]))) {
            const auto fi = types::ffi_closure_id_index(types::as_closure_id(a[1]));
            fin = ev.ffi_runtime_.ffi_func_ptr(fi);
            if (fin == nullptr)
                return f64err("c-handle: finalizer is not a live c-func");
        } else if (!(is_bool(a[1]) && !as_bool(a[1]))) {
            return f64err("c-handle: finalizer must be a c-func procedure or #f");
        }
        std::string tag;
        if (a.size() >= 3) {
            if (!is_string(a[2]))
                return f64err("c-handle: tag must be a string or symbol");
            tag = ev.copy_string_heap_at(static_cast<std::size_t>(as_string_idx(a[2])));
        }
        std::lock_guard lock(ev.alloc_storage_lock_);
        auto& oh = ev.opaque_heap_;
        void* ptr = nullptr;
        if (is_opaque(a[0])) {
            const auto src_packed = types::as_opaque_idx(a[0]);
            std::uint32_t src = 0;
            // A generation mismatch must not wrap the new tenant.
            if (!ev.ffi_runtime_.locate_opaque(oh, src_packed, src))
                return f64err("c-handle: invalid opaque");
            ptr = oh[src];
        } else {
            ptr = reinterpret_cast<void*>(static_cast<std::uintptr_t>(as_int(a[0])));
        }
        if (ptr == nullptr)
            return f64err("c-handle: pointer is null, freed or closed");
        // A handle always mints a new slot. It does not reuse the source
        // opaque and does not update the address alias.
        auto hpacked = ev.ffi_runtime_.mint_opaque_slot(oh, ptr);
        if (hpacked == ~std::uint64_t{0}) {
            const auto slot = static_cast<std::uint32_t>(oh.size());
            oh.push_back(ptr);
            ev.ffi_runtime_.note_appended_opaque(oh);
            hpacked = types::pack_opaque_index(slot, 0);
        }
        // Issue #3022 / #3291: GENERAL_OBJECT_PIN_EXEMPT: external-native-addr
        // (a handle wraps a pointer the native module allocated).
        aura::core::lifetime::note_ffi_opaque_create_exempt("external-native-addr");
        ev.ffi_runtime_.add_native_handle(types::opaque_slot_index(hpacked), fin, std::move(tag));
        return types::make_opaque(hpacked);
    });
    ev.defer_std_host_prim("c-close!", [&ev, f64err](const auto& a) -> EvalValue {
        if (a.empty() || !is_opaque(a[0]))
            return f64err("c-close!: expected a handle (c-handle)");
        void* fin = nullptr;
        void* ptr = nullptr;
        {
            std::lock_guard lock(ev.alloc_storage_lock_);
            const auto packed = types::as_opaque_idx(a[0]);
            // Generation before the null test: a recycled slot's packed index
            // still names the slot, and treating the whole word as an index
            // looks out of range and would raise on the second close.
            std::uint32_t slot = 0;
            if (!ev.ffi_runtime_.locate_opaque(ev.opaque_heap_, packed, slot)) {
                const auto raw = types::opaque_slot_index(packed);
                if (raw >= ev.opaque_heap_.size() && types::opaque_generation_of(packed) == 0)
                    return f64err("c-close!: expected a handle (c-handle)");
                return make_bool(false);
            }
            // A closed (or freed) handle is a null slot: closing again is #f.
            if (ev.opaque_heap_[slot] == nullptr)
                return make_bool(false);
            if (!ev.ffi_runtime_.take_native_handle(ev.opaque_heap_, packed, fin, ptr))
                return f64err("c-close!: expected a handle (c-handle)");
        }
        FFIRuntime::run_native_finalizer(fin, ptr); // outside the lock
        return make_bool(true);
    });
    ev.defer_std_host_prim("c-handle-live?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_opaque(a[0]))
            return make_bool(false);
        std::lock_guard lock(ev.alloc_storage_lock_);
        const auto packed = types::as_opaque_idx(a[0]);
        std::uint32_t slot = 0;
        if (!ev.ffi_runtime_.locate_opaque(ev.opaque_heap_, packed, slot))
            return make_bool(false);
        return make_bool(ev.ffi_runtime_.is_native_handle(packed) &&
                         ev.opaque_heap_[slot] != nullptr);
    });
    // (c-handle-tag h) → tag string ("" when none); #f when h is not an open
    // handle.
    ev.defer_std_host_prim("c-handle-tag", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_opaque(a[0]))
            return make_bool(false);
        std::string tag;
        {
            std::lock_guard lock(ev.alloc_storage_lock_);
            const auto packed = types::as_opaque_idx(a[0]);
            std::uint32_t slot = 0;
            if (!ev.ffi_runtime_.locate_opaque(ev.opaque_heap_, packed, slot) ||
                ev.opaque_heap_[slot] == nullptr)
                return make_bool(false);
            if (!ev.ffi_runtime_.native_handle_tag(packed, tag))
                return make_bool(false);
        }
        return make_string(static_cast<std::uint64_t>(ev.push_string_heap(std::move(tag))));
    });
}

} // namespace aura::compiler::primitives_detail
