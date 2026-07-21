// voxel_volume.hh — Issue #1982 / Epic #1979: dense voxel volume for DDA.
//
// Pure C++ engine storage (no Aura / TUI). Designed for the upcoming
// Amanatides–Woo DDA raycaster (#1979 child).
//
// Coordinate convention (matches camera.hh #1981):
//   - Right-handed, **Y-up**
//   - Integer voxel coords: (x, y, z) with origin at the volume corner
//   - Voxel (x,y,z) occupies the unit cube [x,x+1) × [y,y+1) × [z,z+1)
//
// Memory layout (DDA cache-friendly):
//   index = x + size_x * (y + size_y * z)
//   - **X fastest** — primary step axis is often horizontal X; contiguous
//     runs along a ray that advances in +X stay in cache line longer.
//   - Y next (vertical), Z slowest (depth). Horizontal XZ traversal still
//     has reasonable locality when Y is stable (floor / ceiling walks).
//
// Out-of-bounds: get() returns `oob_block` (default kAir). set() is a no-op.

#ifndef AURA_RENDERER_VOXEL_VOLUME_HH
#define AURA_RENDERER_VOXEL_VOLUME_HH

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::renderer {

inline constexpr int kVoxelVolumeIssue = 1982;
inline constexpr int kVoxelVolumeEpic = 1979;

using BlockId = std::uint16_t;
inline constexpr BlockId kAir = 0;
// Reserved for material tables / solid OOB policy (not a free palette id).
inline constexpr BlockId kBedrock = 1;

// Dense axis-aligned voxel grid.
struct VoxelVolume {
    int size_x = 0;
    int size_y = 0;
    int size_z = 0;
    // X-major: index = x + sx*(y + sy*z)
    std::vector<BlockId> data;
    // Returned by get() when (x,y,z) is outside the volume.
    BlockId oob_block = kAir;

    VoxelVolume() = default;

    // Allocate size_x * size_y * size_z blocks, filled with `fill`.
    // Zero or negative dims → empty (valid() == false).
    explicit VoxelVolume(int sx, int sy, int sz, BlockId fill = kAir) { resize(sx, sy, sz, fill); }

    void resize(int sx, int sy, int sz, BlockId fill = kAir) {
        if (sx <= 0 || sy <= 0 || sz <= 0) {
            size_x = size_y = size_z = 0;
            data.clear();
            return;
        }
        size_x = sx;
        size_y = sy;
        size_z = sz;
        data.assign(static_cast<std::size_t>(sx) * static_cast<std::size_t>(sy) *
                        static_cast<std::size_t>(sz),
                    fill);
    }

    [[nodiscard]] bool valid() const noexcept {
        return size_x > 0 && size_y > 0 && size_z > 0 &&
               data.size() == static_cast<std::size_t>(size_x) * static_cast<std::size_t>(size_y) *
                                  static_cast<std::size_t>(size_z);
    }

    [[nodiscard]] std::size_t block_count() const noexcept { return data.size(); }

    [[nodiscard]] bool in_bounds(int x, int y, int z) const noexcept {
        return x >= 0 && y >= 0 && z >= 0 && x < size_x && y < size_y && z < size_z;
    }

    // Hot path: no branch on fill — OOB → oob_block.
    [[nodiscard]] BlockId get(int x, int y, int z) const noexcept {
        if (!in_bounds(x, y, z))
            return oob_block;
        return data[index_unchecked(x, y, z)];
    }

    // Returns true if written; false if OOB (no write).
    bool set(int x, int y, int z, BlockId id) noexcept {
        if (!in_bounds(x, y, z))
            return false;
        data[index_unchecked(x, y, z)] = id;
        return true;
    }

    void fill(BlockId id) {
        for (auto& b : data)
            b = id;
    }

    void clear() { fill(kAir); }

    // Linear index (X-major). Precondition: in_bounds.
    [[nodiscard]] std::size_t index_unchecked(int x, int y, int z) const noexcept {
        return static_cast<std::size_t>(x) +
               static_cast<std::size_t>(size_x) *
                   (static_cast<std::size_t>(y) +
                    static_cast<std::size_t>(size_y) * static_cast<std::size_t>(z));
    }

    // Decode linear index → (x,y,z). Returns false if i >= block_count().
    [[nodiscard]] bool decode_index(std::size_t i, int& x, int& y, int& z) const noexcept {
        if (i >= data.size() || size_x <= 0 || size_y <= 0)
            return false;
        const auto sx = static_cast<std::size_t>(size_x);
        const auto sy = static_cast<std::size_t>(size_y);
        z = static_cast<int>(i / (sx * sy));
        const std::size_t rem = i - static_cast<std::size_t>(z) * sx * sy;
        y = static_cast<int>(rem / sx);
        x = static_cast<int>(rem - static_cast<std::size_t>(y) * sx);
        return true;
    }
};

// ── Chunked world (Phase 1.5): fixed 16³ chunks, dense chunk grid ──
//
// World block coords still Y-up. A chunk at (cx,cy,cz) covers
//   [cx*S, (cx+1)*S) × [cy*S, (cy+1)*S) × [cz*S, (cz+1)*S)
// Storage: same X-major order inside each chunk; chunks laid out as
//   chunk_index = cx + ncx*(cy + ncy*cz)
// so neighboring chunks along X are adjacent (matches volume layout).

struct ChunkGrid {
    static constexpr int kChunkSize = 16;

    int chunks_x = 0;
    int chunks_y = 0;
    int chunks_z = 0;
    std::vector<BlockId> data; // size = nchunks * kChunkSize³
    BlockId oob_block = kAir;

    ChunkGrid() = default;

    explicit ChunkGrid(int ncx, int ncy, int ncz, BlockId fill = kAir) {
        resize(ncx, ncy, ncz, fill);
    }

    void resize(int ncx, int ncy, int ncz, BlockId fill = kAir) {
        if (ncx <= 0 || ncy <= 0 || ncz <= 0) {
            chunks_x = chunks_y = chunks_z = 0;
            data.clear();
            return;
        }
        chunks_x = ncx;
        chunks_y = ncy;
        chunks_z = ncz;
        const std::size_t per = static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize);
        const std::size_t nch = static_cast<std::size_t>(ncx) * static_cast<std::size_t>(ncy) *
                                static_cast<std::size_t>(ncz);
        data.assign(nch * per, fill);
    }

    [[nodiscard]] bool valid() const noexcept {
        if (chunks_x <= 0 || chunks_y <= 0 || chunks_z <= 0)
            return false;
        const std::size_t per = static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize);
        const std::size_t nch = static_cast<std::size_t>(chunks_x) *
                                static_cast<std::size_t>(chunks_y) *
                                static_cast<std::size_t>(chunks_z);
        return data.size() == nch * per;
    }

    [[nodiscard]] int size_x() const noexcept { return chunks_x * kChunkSize; }
    [[nodiscard]] int size_y() const noexcept { return chunks_y * kChunkSize; }
    [[nodiscard]] int size_z() const noexcept { return chunks_z * kChunkSize; }

    [[nodiscard]] bool in_bounds(int x, int y, int z) const noexcept {
        return x >= 0 && y >= 0 && z >= 0 && x < size_x() && y < size_y() && z < size_z();
    }

    [[nodiscard]] BlockId get(int x, int y, int z) const noexcept {
        if (!in_bounds(x, y, z))
            return oob_block;
        return data[index_world(x, y, z)];
    }

    bool set(int x, int y, int z, BlockId id) noexcept {
        if (!in_bounds(x, y, z))
            return false;
        data[index_world(x, y, z)] = id;
        return true;
    }

    void fill(BlockId id) {
        for (auto& b : data)
            b = id;
    }

    // World → linear index (chunk-major, X-major inside chunk).
    [[nodiscard]] std::size_t index_world(int x, int y, int z) const noexcept {
        const int cx = x / kChunkSize;
        const int cy = y / kChunkSize;
        const int cz = z / kChunkSize;
        const int lx = x - cx * kChunkSize;
        const int ly = y - cy * kChunkSize;
        const int lz = z - cz * kChunkSize;
        const std::size_t per = static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize) *
                                static_cast<std::size_t>(kChunkSize);
        const std::size_t cidx =
            static_cast<std::size_t>(cx) +
            static_cast<std::size_t>(chunks_x) *
                (static_cast<std::size_t>(cy) +
                 static_cast<std::size_t>(chunks_y) * static_cast<std::size_t>(cz));
        const std::size_t lidx =
            static_cast<std::size_t>(lx) +
            static_cast<std::size_t>(kChunkSize) *
                (static_cast<std::size_t>(ly) +
                 static_cast<std::size_t>(kChunkSize) * static_cast<std::size_t>(lz));
        return cidx * per + lidx;
    }
};

// Fill a solid axis-aligned box [x0,x1) × [y0,y1) × [z0,z1) (half-open).
// Clips to volume bounds. Returns number of voxels written.
inline std::size_t voxel_fill_box(VoxelVolume& vol, int x0, int y0, int z0, int x1, int y1, int z1,
                                  BlockId id) noexcept {
    if (!vol.valid())
        return 0;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    z0 = z0 < 0 ? 0 : z0;
    x1 = x1 > vol.size_x ? vol.size_x : x1;
    y1 = y1 > vol.size_y ? vol.size_y : y1;
    z1 = z1 > vol.size_z ? vol.size_z : z1;
    std::size_t n = 0;
    for (int z = z0; z < z1; ++z) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                vol.data[vol.index_unchecked(x, y, z)] = id;
                ++n;
            }
        }
    }
    return n;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_VOXEL_VOLUME_HH
