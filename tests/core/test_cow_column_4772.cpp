// Issue #4772: unit tests for core/cow_column.hh — the chunk-tracked SoA
// column + structural-sharing image that make ast:snapshot / ast:restore
// O(changed chunks) instead of an O(workspace) deep copy. Header-only, no
// modules: exercises the soundness rules documented in the header.
#include "core/cow_column.hh"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory_resource>
#include <random>
#include <unordered_map>
#include <vector>

namespace cow = aura::ast::cow;

static int g_fail = 0;
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                   \
            ++g_fail;                                                                              \
        } else {                                                                                   \
            std::printf("  ok: %s\n", msg);                                                        \
        }                                                                                          \
    } while (0)

using Col = cow::TrackedColumn<std::uint32_t>;
constexpr std::size_t K = Col::kChunk;

static std::size_t shared_chunks(const Col::image_type& a, const Col::image_type& b) {
    std::size_t n = 0;
    for (std::size_t c = 0; c < a.chunks.size() && c < b.chunks.size(); ++c)
        n += a.chunks[c] == b.chunks[c];
    return n;
}

static void capture_shares_untouched_chunks() {
    std::puts("--- capture shares untouched chunks ---");
    Col col;
    for (std::uint32_t i = 0; i < 100 * K; ++i)
        col.push_back(i);
    std::size_t fresh = 0, total = 0;
    auto img0 = col.capture(nullptr, &fresh, &total);
    CHECK(fresh == 100 && total == 100, "first capture copies every chunk");
    col[5 * K + 3] = 7; // one write
    fresh = total = 0;
    auto img1 = col.capture(&img0, &fresh, &total);
    CHECK(fresh == 1, "one write -> one fresh chunk");
    CHECK(shared_chunks(img0, img1) == 99, "99 of 100 chunks shared by pointer");
    CHECK(img0.at(5 * K + 3) == 5 * K + 3, "older image is immutable");
    CHECK(img1.at(5 * K + 3) == 7, "new image holds the write");
    // A non-const read marks, but equal bytes are still shared.
    (void)col[9 * K];
    fresh = 0;
    auto img2 = col.capture(&img1, &fresh, nullptr);
    CHECK(fresh == 0, "written-but-equal chunk is shared (content compare)");
    // Append grows only the tail.
    col.push_back(1u);
    fresh = 0;
    auto img3 = col.capture(&img2, &fresh, nullptr);
    CHECK(fresh == 1 && img3.size == 100 * K + 1, "append copies only the new tail chunk");
}

static void restore_is_delta_and_exact() {
    std::puts("--- restore is delta + exact ---");
    Col col;
    for (std::uint32_t i = 0; i < 64 * K; ++i)
        col.push_back(i * 3);
    auto img0 = col.capture(nullptr);
    col[1] = 11;
    col[40 * K] = 12;
    col.resize(70 * K, 9u);
    auto& st = cow::cow_stats();
    const auto copied0 = st.restore_chunks_copied.load();
    const auto skipped0 = st.restore_chunks_skipped.load();
    col.restore_from(img0, &img0);
    CHECK(col.equals_image(img0), "restore yields exactly the image");
    CHECK(col.size() == 64 * K, "restore shrinks a grown column");
    CHECK(st.restore_chunks_copied.load() - copied0 == 2, "only the 2 written chunks copied back");
    CHECK(st.restore_chunks_skipped.load() - skipped0 == 62, "62 clean chunks skipped");
    // Restore an OLDER image after another capture (worldline jump).
    col[2] = 99;
    auto img1 = col.capture(&img0);
    col.restore_from(img0, &img1);
    CHECK(col.equals_image(img0), "jump back to an older image");
    col.restore_from(img1, &img0);
    CHECK(col.equals_image(img1) && col[2] == 99, "jump forward again");
}

static void wholesale_writes_fall_back_to_compare() {
    std::puts("--- wholesale writes are covered ---");
    Col col;
    for (std::uint32_t i = 0; i < 8 * K; ++i)
        col.push_back(i);
    auto img0 = col.capture(nullptr);
    // Raw pointer write: data() marks all-dirty.
    col.data()[3 * K] = 1234;
    std::size_t fresh = 0;
    auto img1 = col.capture(&img0, &fresh, nullptr);
    CHECK(fresh == 1 && img1.at(3 * K) == 1234, "data() write captured; equal chunks shared");
    // Copy-assign from a plain vector.
    std::pmr::vector<std::uint32_t> v(8 * K, 5u);
    col = v;
    auto img2 = col.capture(&img1);
    CHECK(img2.at(0) == 5 && img2.at(8 * K - 1) == 5, "assign from raw vector captured");
    Col other;
    other = col;
    auto img3 = other.capture(&img2);
    CHECK(other.equals_image(img3) && img3.at(17) == 5, "copy-assigned column captures exactly");
    col.restore_from(img0, &img2);
    CHECK(col.equals_image(img0), "restore after wholesale writes");
}

static void randomized_oracle() {
    std::puts("--- randomized oracle vs deep copies ---");
    std::mt19937 rng(4772);
    Col col;
    std::vector<std::vector<std::uint32_t>> deep;
    std::vector<Col::image_type> imgs;
    const Col::image_type* base = nullptr;
    Col::image_type last;
    bool ok = true;
    for (int step = 0; step < 300; ++step) {
        const int op = static_cast<int>(rng() % 10);
        if (op < 5 && !col.empty()) {
            col[rng() % col.size()] = rng();
        } else if (op < 7) {
            for (int k = 0; k < static_cast<int>(rng() % 50); ++k)
                col.push_back(rng());
        } else if (op < 8 && col.size() > 10) {
            col.resize(col.size() - rng() % 10);
        } else if (!imgs.empty()) {
            const auto pick = rng() % imgs.size();
            col.restore_from(imgs[pick], base);
            ok = ok && col.equals_image(imgs[pick]);
            last = imgs[pick];
            base = &last;
        }
        if (step % 7 == 0) {
            auto img = col.capture(base);
            std::vector<std::uint32_t> d(col.begin(), col.end());
            ok = ok && img.size == d.size();
            for (std::size_t i = 0; ok && i < d.size(); ++i)
                ok = img.at(i) == d[i];
            imgs.push_back(img);
            deep.push_back(std::move(d));
            last = imgs.back();
            base = &last;
        }
    }
    for (std::size_t i = 0; ok && i < imgs.size(); ++i)
        for (std::size_t j = 0; ok && j < deep[i].size(); ++j)
            ok = imgs[i].at(j) == deep[i][j];
    CHECK(ok, "300 random edits/captures/restores: every image equals its deep copy");
}

static void tracked_map() {
    std::puts("--- tracked map ---");
    cow::TrackedMap<std::unordered_map<std::uint32_t, std::uint8_t>> m;
    m[1] = 2;
    auto i0 = m.capture(nullptr);
    std::size_t fresh = 0;
    auto i1 = m.capture(&i0, &fresh, nullptr);
    CHECK(fresh == 0 && i1 == i0, "unchanged map shares its image");
    m[3] = 4;
    auto i2 = m.capture(&i1);
    CHECK(i2 != i1 && i2->size() == 2 && i1->size() == 1, "changed map copies; old image frozen");
    m.restore_from(i1, &i2);
    CHECK(m.equals_image(i1), "map restore");
}

int main() {
    capture_shares_untouched_chunks();
    restore_is_delta_and_exact();
    wholesale_writes_fall_back_to_compare();
    randomized_oracle();
    tracked_map();
    if (g_fail) {
        std::fprintf(stderr, "test_cow_column_4772: %d failure(s)\n", g_fail);
        return 1;
    }
    std::puts("test_cow_column_4772: all passed");
    return 0;
}
