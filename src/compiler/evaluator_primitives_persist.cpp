// evaluator_primitives_persist.cpp — Issue #1381:
// serialize-workspace / deserialize-workspace / workspace-persist-info
// Binary format: AURASOUL\x01 magic + format version + CRC32 trailer.

module;

#include "aura_jit_bridge.h"
#include "hash_meta.h"
#include "runtime_shared.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.mutation;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using types::as_int;
using types::as_pair_idx;
using types::as_string_idx;
using types::is_error;
using types::is_int;
using types::is_pair;
using types::is_string;
using types::make_bool;
using types::make_hash;
using types::make_int;
using types::make_string;
using types::make_void;

// Magic "AURASOUL" + 0x01 marker (issue AC).
static constexpr char kMagic[8] = {'A', 'U', 'R', 'A', 'S', 'O', 'U', 'L'};
static constexpr std::uint8_t kMagicMark = 0x01;
// Issue #4365: v2 appends author_fingerprint, parent_mutation_id, and
// composite_transaction_id on each mutation record. v1 blobs still load;
// those three fields stay 0.
static constexpr std::uint32_t kFormatVersion = 2;
static constexpr std::uint32_t kFormatVersionMin = 1;

static constexpr std::uint32_t kSecSource = 1;
static constexpr std::uint32_t kSecMeta = 2;
static constexpr std::uint32_t kSecMutations = 3;
static constexpr std::uint32_t kSecEnvPlaceholder = 4; // reserved env_frames
// Issue #4367: optional. One define name per mutation record (empty when
// the saved target was not a Define). Old blobs omit the section.
static constexpr std::uint32_t kSecDefineTargets = 5;
static constexpr std::uint32_t kSecEnd = 0xFFFFFFFFu;

static std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t len) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            std::uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void append_u32(std::vector<char>& buf, std::uint32_t v) {
    buf.insert(buf.end(), reinterpret_cast<const char*>(&v), reinterpret_cast<const char*>(&v) + 4);
}
static void append_u64(std::vector<char>& buf, std::uint64_t v) {
    buf.insert(buf.end(), reinterpret_cast<const char*>(&v), reinterpret_cast<const char*>(&v) + 8);
}
static void append_bytes(std::vector<char>& buf, const void* data, std::size_t n) {
    const auto* p = static_cast<const char*>(data);
    buf.insert(buf.end(), p, p + n);
}

static bool read_u32(const std::vector<char>& buf, std::size_t& pos, std::uint32_t& out) {
    if (pos + 4 > buf.size())
        return false;
    std::memcpy(&out, buf.data() + pos, 4);
    pos += 4;
    return true;
}
static bool read_u64(const std::vector<char>& buf, std::size_t& pos, std::uint64_t& out) {
    if (pos + 8 > buf.size())
        return false;
    std::memcpy(&out, buf.data() + pos, 8);
    pos += 8;
    return true;
}

struct PersistBlob {
    std::uint32_t format_version = 0;
    std::uint64_t module_version = 0;
    std::uint64_t region_mask = 0;
    std::uint64_t bridge_epoch = 0;
    std::uint64_t mutation_count = 0;
    std::string source;
    std::vector<char> raw;
    std::size_t mutations_data_pos = 0;
    std::uint32_t mutations_count = 0;
    // Parallel to the mutation log. Empty when the blob has no #4367 section.
    std::vector<std::string> define_targets;
    bool magic_ok = false;
    bool crc_ok = false;
};

static bool load_blob(const std::string& path, PersistBlob& blob, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err)
            *err = "open failed";
        return false;
    }
    blob.raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (blob.raw.size() < 20) {
        if (err)
            *err = "file too small";
        return false;
    }
    if (std::memcmp(blob.raw.data(), kMagic, 8) != 0 ||
        static_cast<std::uint8_t>(blob.raw[8]) != kMagicMark) {
        blob.magic_ok = false;
        if (err)
            *err = "bad magic (expected AURASOUL\\x01)";
        return false;
    }
    blob.magic_ok = true;
    std::size_t pos = 9;
    if (!read_u32(blob.raw, pos, blob.format_version)) {
        if (err)
            *err = "truncated header";
        return false;
    }
    // Issue #4365: accept v1 (no provenance tail) through the current writer.
    if (blob.format_version < kFormatVersionMin || blob.format_version > kFormatVersion) {
        if (err)
            *err = "unsupported format version " + std::to_string(blob.format_version);
        return false;
    }
    std::uint64_t flags = 0;
    if (!read_u64(blob.raw, pos, flags)) {
        if (err)
            *err = "truncated flags";
        return false;
    }
    (void)flags;

    const std::size_t crc_pos = blob.raw.size() - 4;
    if (crc_pos < pos) {
        if (err)
            *err = "missing CRC";
        return false;
    }
    std::uint32_t file_crc = 0;
    std::memcpy(&file_crc, blob.raw.data() + crc_pos, 4);
    blob.crc_ok = (crc32_update(0, blob.raw.data(), crc_pos) == file_crc);
    if (!blob.crc_ok) {
        if (err)
            *err = "CRC32 mismatch";
        return false;
    }

    while (pos + 4 <= crc_pos) {
        std::uint32_t tag = 0;
        if (!read_u32(blob.raw, pos, tag))
            break;
        if (tag == kSecEnd)
            break;
        std::uint32_t len = 0;
        if (!read_u32(blob.raw, pos, len) || pos + len > crc_pos) {
            if (err)
                *err = "truncated section";
            return false;
        }
        if (tag == kSecMeta) {
            std::size_t sp = pos;
            if (!read_u64(blob.raw, sp, blob.module_version) ||
                !read_u64(blob.raw, sp, blob.region_mask) ||
                !read_u64(blob.raw, sp, blob.bridge_epoch) ||
                !read_u64(blob.raw, sp, blob.mutation_count)) {
                if (err)
                    *err = "bad meta section";
                return false;
            }
        } else if (tag == kSecSource) {
            blob.source.assign(blob.raw.data() + pos, len);
        } else if (tag == kSecMutations) {
            if (len < 4) {
                if (err)
                    *err = "bad mutations section";
                return false;
            }
            std::memcpy(&blob.mutations_count, blob.raw.data() + pos, 4);
            blob.mutations_data_pos = pos + 4;
        } else if (tag == kSecDefineTargets) {
            // Best-effort. A short section leaves names empty and skips remap.
            std::size_t sp = pos;
            std::uint32_t count = 0;
            std::vector<std::string> names;
            bool names_ok = read_u32(blob.raw, sp, count);
            if (names_ok) {
                names.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t n = 0;
                    if (!read_u32(blob.raw, sp, n) || sp + n > pos + len) {
                        names_ok = false;
                        break;
                    }
                    names.emplace_back(blob.raw.data() + sp, n);
                    sp += n;
                }
            }
            if (names_ok)
                blob.define_targets = std::move(names);
        }
        pos += len;
    }
    return true;
}

// Issue #4367: name of the define at `id`, or empty when the slot is not a
// live Define. The caller copies the string; the pool view is not kept.
static std::string define_name_at(aura::ast::StringPool* pool, const aura::ast::FlatAST& flat,
                                  aura::ast::NodeId id) {
    if (!pool || id == aura::ast::NULL_NODE || id >= flat.size() || flat.is_free_slot(id))
        return {};
    const auto v = flat.get(id);
    if (v.tag != aura::ast::NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
        return {};
    const auto sv = pool->resolve(v.sym_id);
    return std::string(sv);
}

// Issue #4367: set-code reparses, so define ids move. Retarget only records
// whose saved target was itself a Define (empty name means leave the id).
static void remap_define_mutation_targets(aura::ast::StringPool* pool, aura::ast::FlatAST& flat,
                                          const std::vector<std::string>& names) {
    if (!pool || names.empty())
        return;
    auto& log = flat.all_mutations();
    if (log.empty())
        return;
    const auto root = flat.root;
    if (root == aura::ast::NULL_NODE || root >= flat.size() || flat.is_free_slot(root))
        return;
    std::unordered_map<std::string, aura::ast::NodeId> by_name;
    std::vector<unsigned char> seen(static_cast<std::size_t>(flat.size()), 0);
    std::vector<aura::ast::NodeId> pending;
    pending.push_back(root);
    while (!pending.empty()) {
        const auto cur = pending.back();
        pending.pop_back();
        if (cur == aura::ast::NULL_NODE || cur >= flat.size() ||
            seen[static_cast<std::size_t>(cur)] || flat.is_free_slot(cur))
            continue;
        seen[static_cast<std::size_t>(cur)] = 1;
        const auto v = flat.get(cur);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM) {
            const auto sv = pool->resolve(v.sym_id);
            if (!sv.empty()) {
                std::string nm(sv);
                auto it = by_name.find(nm);
                // define-lookup prepends, so its car is the highest id.
                if (it == by_name.end() || cur > it->second)
                    by_name.insert_or_assign(std::move(nm), cur);
            }
        }
        for (auto child : v.children)
            pending.push_back(child);
    }
    const auto nrec = log.size() < names.size() ? log.size() : names.size();
    for (std::size_t i = 0; i < nrec; ++i) {
        if (names[i].empty())
            continue;
        const auto it = by_name.find(names[i]);
        if (it != by_name.end())
            log[i].target_node = it->second;
    }
}

void register_persist_primitives(PrimRegistrar add, Evaluator& ev) {
    // (serialize-workspace path) → #t / #f
    add("serialize-workspace", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        const auto pidx = as_string_idx(a[0]);
        if (pidx >= ev.string_heap_.size())
            return make_bool(false);
        const std::string path = ev.string_heap_[pidx];

        // Shared lock: pure read of workspace + mutation log (#1376 pattern).
        // Issue #2920 SSOT: authoritative source (live unparse preferred;
        // never stale pre-mutate workspace_source_text_ after Guard exit).
        ev.lock_workspace_shared();
        std::string source = ev.authoritative_workspace_source();
        std::uint64_t mut_count = 0;
        std::vector<char> mut_blob;
        std::vector<std::string> define_names;
        if (ev.workspace_flat_) {
            mut_count = static_cast<std::uint64_t>(ev.workspace_flat_->mutation_count());
            const auto& log = ev.workspace_flat_->all_mutations();
            append_u32(mut_blob, static_cast<std::uint32_t>(log.size()));
            define_names.reserve(log.size());
            for (const auto& rec : log) {
                aura::ast::mutation::wire_write_mutation_record(
                    mut_blob, rec, aura::ast::mutation::kMutationRecordProvenanceWireVersion);
                define_names.push_back(
                    define_name_at(ev.workspace_pool_, *ev.workspace_flat_, rec.target_node));
            }
        } else {
            append_u32(mut_blob, 0);
        }
        const std::uint64_t module_ver = aura_get_module_version_for_eval(&ev);
        const std::uint64_t region = aura_get_aot_region_mask_for_eval(&ev);
        const std::uint64_t bridge = ev.current_bridge_epoch();
        ev.unlock_workspace_shared();

        std::vector<char> out;
        out.reserve(256 + source.size() + mut_blob.size());
        append_bytes(out, kMagic, 8);
        out.push_back(static_cast<char>(kMagicMark));
        append_u32(out, kFormatVersion);
        append_u64(out, 0); // flags

        // Meta
        {
            std::vector<char> meta;
            append_u64(meta, module_ver);
            append_u64(meta, region);
            append_u64(meta, bridge);
            append_u64(meta, mut_count);
            append_u32(out, kSecMeta);
            append_u32(out, static_cast<std::uint32_t>(meta.size()));
            append_bytes(out, meta.data(), meta.size());
        }
        // Source
        append_u32(out, kSecSource);
        append_u32(out, static_cast<std::uint32_t>(source.size()));
        append_bytes(out, source.data(), source.size());
        // Mutations
        append_u32(out, kSecMutations);
        append_u32(out, static_cast<std::uint32_t>(mut_blob.size()));
        append_bytes(out, mut_blob.data(), mut_blob.size());
        // Issue #4367: define name for each record, so reload can retarget.
        {
            std::vector<char> sec;
            append_u32(sec, static_cast<std::uint32_t>(define_names.size()));
            for (const auto& name : define_names) {
                append_u32(sec, static_cast<std::uint32_t>(name.size()));
                append_bytes(sec, name.data(), name.size());
            }
            append_u32(out, kSecDefineTargets);
            append_u32(out, static_cast<std::uint32_t>(sec.size()));
            append_bytes(out, sec.data(), sec.size());
        }
        // Env placeholder (Phase 2 env_frames SoA)
        append_u32(out, kSecEnvPlaceholder);
        append_u32(out, 0);
        // End (tag only — no length payload)
        append_u32(out, kSecEnd);

        const std::uint32_t crc = crc32_update(0, out.data(), out.size());
        append_u32(out, crc);

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return make_bool(false);
        ofs.write(out.data(), static_cast<std::streamsize>(out.size()));
        return make_bool(static_cast<bool>(ofs));
    });

    // (deserialize-workspace path) → #t / #f
    add("deserialize-workspace", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        const auto pidx = as_string_idx(a[0]);
        if (pidx >= ev.string_heap_.size())
            return make_bool(false);
        const std::string path = ev.string_heap_[pidx];
        std::string err;
        PersistBlob blob;
        if (!load_blob(path, blob, &err))
            return make_bool(false);

        // set-code manages its own workspace lock — call outside our lock.
        bool eval_rejected = false;
        if (!blob.source.empty()) {
            // Issue #1397: string_heap_ push_back atomic — only the heap
            // mutation is under alloc_storage_lock_; set-code / eval-current
            // take their own workspace locks and must not nest under it.
            std::uint32_t sidx = 0;
            {
                std::lock_guard lock(ev.alloc_storage_lock_);
                sidx = static_cast<std::uint32_t>(ev.string_heap_.size());
                ev.string_heap_.push_back(blob.source);
            }
            auto set_fn = ev.primitives_.lookup("set-code");
            if (!set_fn)
                return make_bool(false);
            auto r = (*set_fn)({make_string(sidx)});
            // set-code returns merr pair on parse failure
            if (is_pair(r) || is_error(r))
                return make_bool(false);
            // Issue #2731: set-code restores FlatAST + IR dirty/pre-cache but
            // leaves top_env cells pointing at pre-load rebind closures.
            // Re-eval so string + SymId bindings match the restored source
            // (aligns file-soul with ast:snapshot/restore live semantics).
            // Issue #4366: the program value may be a list. make_merr is
            // (kind . (message . 0)); a normal pair is not a failure.
            // A real eval error still restores the log below, then returns #f.
            if (auto eval_fn = ev.primitives_.lookup("eval-current")) {
                auto er = (*eval_fn)({});
                auto merr = [&ev](const EvalValue& v) -> bool {
                    if (!is_pair(v))
                        return false;
                    const auto oi = as_pair_idx(v);
                    if (oi >= ev.pairs_.size())
                        return false;
                    const auto& outer = ev.pairs_[oi];
                    if (!is_string(outer.car) || !is_pair(outer.cdr))
                        return false;
                    const auto ii = as_pair_idx(outer.cdr);
                    if (ii >= ev.pairs_.size())
                        return false;
                    const auto& inner = ev.pairs_[ii];
                    return is_string(inner.car) && is_int(inner.cdr) && as_int(inner.cdr) == 0;
                };
                if (is_error(er) || merr(er))
                    eval_rejected = true;
            }
        }

        // Restore mutation log (audit trail) onto workspace flat.
        if (ev.workspace_flat_ && blob.mutations_count > 0) {
            ev.lock_workspace_unique();
            auto& log = ev.workspace_flat_->all_mutations();
            log.clear();
            std::size_t pos = blob.mutations_data_pos;
            for (std::uint32_t i = 0; i < blob.mutations_count; ++i) {
                if (pos + 8 > blob.raw.size())
                    break;
                try {
                    const auto wire_ver =
                        blob.format_version >= 2
                            ? aura::ast::mutation::kMutationRecordProvenanceWireVersion
                            : 1u;
                    auto rec =
                        aura::ast::mutation::wire_read_mutation_record(blob.raw, pos, wire_ver);
                    log.push_back(std::move(rec));
                } catch (...) {
                    // [SILENCE-PRIM-#615] truncated/corrupt mutation wire
                    // record — stop loading trail; partial audit is best
                    // effort (#1669 class B intentional-state).
                    break;
                }
            }
            // Issue #4367: names were captured on the pre-save tree.
            remap_define_mutation_targets(ev.workspace_pool_, *ev.workspace_flat_,
                                          blob.define_targets);
            ev.unlock_workspace_unique();
        }

        aura_set_module_version_for_eval(&ev, blob.module_version);
        aura_set_aot_region_mask_for_eval(&ev, blob.region_mask);
        // Issue #4366: code is already the saved program. Returning #f
        // before the log restore left a half-restored workspace.
        if (eval_rejected)
            return make_bool(false);
        return make_bool(true);
    });

    // (workspace-persist-info path) → hash | #f
    add("workspace-persist-info", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        const auto pidx = as_string_idx(a[0]);
        if (pidx >= ev.string_heap_.size())
            return make_bool(false);
        PersistBlob blob;
        std::string err;
        if (!load_blob(ev.string_heap_[pidx], blob, &err))
            return make_bool(false);

        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_bool(false);
        auto put = [&](const char* k, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::hash::kFnvOffsetBasis;
            for (const char* p = k; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::hash::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            // Issue #1397: string_heap_ push_back atomic — hoist the
            // size() + push_back() out of an inner block so `kidx` is
            // visible to the `make_string(kidx).val` call below.
            std::lock_guard lock(ev.alloc_storage_lock_);
            const std::size_t kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = make_string(kidx).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        put("format-version", static_cast<std::int64_t>(blob.format_version));
        put("module-version", static_cast<std::int64_t>(blob.module_version));
        put("region-mask", static_cast<std::int64_t>(blob.region_mask));
        put("bridge-epoch", static_cast<std::int64_t>(blob.bridge_epoch));
        put("mutation-count", static_cast<std::int64_t>(blob.mutation_count));
        put("source-bytes", static_cast<std::int64_t>(blob.source.size()));
        put("mutations-recorded", static_cast<std::int64_t>(blob.mutations_count));
        put("magic-ok", blob.magic_ok ? 1 : 0);
        put("crc-ok", blob.crc_ok ? 1 : 0);
        put("schema", 1381);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        // Issue #4110: stamp the owner parallel to the table (SSOT helper; runtime_ssot contract).
        aura_hash_stamp_new_table_owner();
        return make_hash(hidx);
    });

    add("workspace-persist-format-version", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(kFormatVersion));
    });
}

} // namespace aura::compiler::primitives_detail
