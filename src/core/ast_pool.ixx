export module aura.core.ast_pool;
import std;
import aura.core;
import aura.core.ast;

namespace aura::ast {

// ── StringPool — arena-backed string interner ──────────────────
//
// Interns strings into a contiguous character buffer and returns
// SymId (std::uint32_t offset into the buffer). The hash table uses
// open addressing with linear probing for cache-friendliness.
//
// All storage is arena-allocated via pmr vector. reset() reclaims
// everything.
//
export class StringPool {
public:
    explicit StringPool(std::pmr::polymorphic_allocator<std::byte> alloc = {})
        : buf_(alloc), hash_tbl_(alloc)
    {
        // Reserve 0 as invalid sentinel
        buf_.push_back('\0');
        // Initialize hash table (power of 2, start small)
        rehash(64);
    }

    // Intern a string — returns a stable SymId
    SymId intern(std::string_view s) {
        if (s.empty()) return 0;

        auto hash = hash_str(s);
        auto mask = hash_capacity_ - 1;
        auto idx = hash & mask;

        while (hash_tbl_[idx] != INVALID_SYM) {
            auto existing = hash_tbl_[idx];
            auto view = resolve(existing);
            if (view == s) return existing;  // already interned
            idx = (idx + 1) & mask;           // linear probe
        }

        // Not found — intern
        auto offset = static_cast<SymId>(buf_.size());
        buf_.insert(buf_.end(), s.begin(), s.end());
        buf_.push_back('\0');  // null terminator

        hash_tbl_[idx] = offset;

        // Grow if load factor > 0.5
        ++entry_count_;
        if (entry_count_ * 2 > hash_capacity_)
            rehash(hash_capacity_ * 2);

        return offset;
    }

    // Resolve a SymId back to string_view
    std::string_view resolve(SymId id) const {
        if (id >= buf_.size()) return {};
        return std::string_view(buf_.data() + id);
    }

    // Total bytes used for string data
    std::size_t data_size() const { return buf_.size(); }

    // Reset all state
    void reset() {
        buf_.clear();
        buf_.push_back('\0');
        hash_tbl_.clear();
        hash_capacity_ = 0;
        entry_count_ = 0;
        rehash(64);
    }

private:
    static std::uint64_t hash_str(std::string_view s) {
        // FNV-1a
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (auto c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001B3ull;
        }
        return h;
    }

    void rehash(std::uint32_t new_cap) {
        auto old_tbl = std::move(hash_tbl_);
        auto old_cap = hash_capacity_;

        hash_capacity_ = new_cap;
        hash_tbl_.assign(new_cap, INVALID_SYM);
        entry_count_ = 0;

        for (std::uint32_t i = 0; i < old_cap; ++i) {
            if (old_tbl[i] != INVALID_SYM) {
                auto hash = hash_str(resolve(old_tbl[i]));
                auto mask = hash_capacity_ - 1;
                auto idx = hash & mask;
                while (hash_tbl_[idx] != INVALID_SYM)
                    idx = (idx + 1) & mask;
                hash_tbl_[idx] = old_tbl[i];
                ++entry_count_;
            }
        }
    }

    // Backing store: contiguous null-terminated strings
    std::pmr::vector<char> buf_;

    // Open-addressing hash table (SymId → offset into buf_)
    std::pmr::vector<SymId> hash_tbl_;
    std::uint32_t hash_capacity_ = 0;
    std::uint32_t entry_count_ = 0;
};

} // namespace aura::ast
