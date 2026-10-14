// evaluator_primitives_char.cpp — P0 step 30: char? / string->list / read-line primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "primitives_detail.h"
#include "runtime_shared.h"
#include "security_side_effect.hh" // kEffectMutate (#4503 f64vector-set!)
#include "messaging_bridge.h"
#include "utf8_scalar.hh" // Issue #4472

#include <poll.h>
#include <termios.h>
#include <unistd.h>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

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
            auto car = ev.pairs_[idx].car;
            if (is_int(car))
                utf8_append(result, as_int(car)); // Issue #4472
            v = ev.pairs_[idx].cdr;
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    // Issue #4325: a blank line is "". EOF (or a failed stream) is void,
    // which eof-object? already treats as the eof value. A partial line
    // with no trailing newline is still that line; the next read is eof.
    // Issue #4356: the blocking read runs without the CLI body mutex so a
    // thread fiber can allocate. The string_heap_ push happens only after
    // the lock is back, on this thread's private line buffer.
    add("read-line", [&ev](const auto&) {
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
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // Issue #4358: non-blocking stdin poll. #t when a later read-line or
    // read-byte would not block (byte already buffered, or the fd is
    // readable, including hangup/EOF). Does not consume the byte.
    // Timeout 0, so the CLI body lock stays held.
    add("char-ready?", [](const auto&) {
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
    add("read-byte", [](const auto&) {
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
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("%base64-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
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
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("%hex-encode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        static const char kHex[] = "0123456789abcdef";
        const std::string& data = ev.string_heap_[idx];
        std::string out;
        out.reserve(data.size() * 2);
        for (unsigned char c : data) {
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
        return make_string(sid);
    });

    add("%hex-decode", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        const auto idx = types::as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
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
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(out));
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
    add("f64vector-length", [f64err, f64slot](std::span<const EvalValue> a) -> EvalValue {
        const auto bytes = a.empty() ? -1 : f64slot(a[0]);
        if (bytes < 0)
            return f64err("f64vector-length: expected an f64vector");
        return make_int(bytes / 8);
    });
    add("f64vector-ref", [f64err, &ev, rd](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]))
            return f64err("f64vector-ref: expected (f64vector index)");
        const auto i = as_int(a[1]);
        // Issue #4509: copy the element out under alloc_storage_lock_ — the
        // heap can reallocate between validation and read on another fiber.
        std::lock_guard lock(ev.alloc_storage_lock_);
        const auto idx = static_cast<std::size_t>(types::as_string_idx(a[0]));
        if (idx >= ev.string_heap_.size() || ev.string_heap_[idx].size() % 8 != 0)
            return f64err("f64vector-ref: expected (f64vector index)");
        const auto& s = ev.string_heap_[idx];
        const auto n = static_cast<std::int64_t>(s.size() / 8);
        if (i < 0 || i >= n)
            return f64err(std::format("f64vector-ref: index {} out of range for length {}", i, n));
        return make_float(rd(s, static_cast<std::size_t>(i)));
    });
    add("f64vector-set!",
        [&ev, f64err, f64slot, as_real](std::span<const EvalValue> a) -> EvalValue {
            double x = 0.0;
            if (a.size() < 3 || f64slot(a[0]) < 0 || !is_int(a[1]) || !as_real(a[2], x))
                return f64err("f64vector-set!: expected (f64vector index number)");
            std::lock_guard lock(ev.alloc_storage_lock_);
            const auto idx = types::as_string_idx(a[0]);
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
            return make_void();
        });
    // (f64vector-dot a b [a-off b-off n]) — sum of a[a-off+k]*b[b-off+k].
    add("f64vector-dot", [f64err, &ev, rd](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return f64err("f64vector-dot: expected two f64vectors");
        // Issue #4509: validate both slots and run the MAC scan under
        // alloc_storage_lock_ — the loop previously held &string_heap_[idx]
        // for the whole dot while a concurrent mint (push_string_heap) could
        // reallocate the heap and free the buffer under it. The pointers
        // stay inside the locked region; nothing is published.
        std::lock_guard lock(ev.alloc_storage_lock_);
        const auto xidx = static_cast<std::size_t>(types::as_string_idx(a[0]));
        const auto yidx = static_cast<std::size_t>(types::as_string_idx(a[1]));
        if (xidx >= ev.string_heap_.size() || ev.string_heap_[xidx].size() % 8 != 0 ||
            yidx >= ev.string_heap_.size() || ev.string_heap_[yidx].size() % 8 != 0)
            return f64err("f64vector-dot: expected two f64vectors");
        const auto& x = ev.string_heap_[xidx];
        const auto& y = ev.string_heap_[yidx];
        const auto nx = static_cast<std::int64_t>(x.size() / 8);
        const auto ny = static_cast<std::int64_t>(y.size() / 8);
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
            acc +=
                rd(x, static_cast<std::size_t>(xo + k)) * rd(y, static_cast<std::size_t>(yo + k));
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
                src = ev.string_heap_[idx];
            }
            return from_bytes(a, src, 4, "f32-bytes->f64vector");
        });
    // (bytes-i32-ref str byte-offset) — little-endian int32 (checkpoint headers).
    add("bytes-i32-ref", [&ev, f64err](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_int(a[1]) ||
            types::as_string_idx(a[0]) >= ev.string_heap_.size())
            return f64err("bytes-i32-ref: expected (string byte-offset)");
        const auto& s = ev.string_heap_[types::as_string_idx(a[0])];
        const auto off = as_int(a[1]);
        if (off < 0 || static_cast<std::size_t>(off) + 4 > s.size())
            return f64err("bytes-i32-ref: offset out of range");
        std::int32_t v;
        std::memcpy(&v, s.data() + off, 4);
        return make_int(v);
    });
}

} // namespace aura::compiler::primitives_detail
