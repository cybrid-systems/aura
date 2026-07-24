// test_voxel_volume.cpp — Issue #1982 / Epic #1979
// Dense voxel volume + chunk grid (headless, no Aura).
//
// ACs:
//   AC1: resize / valid / block_count
//   AC2: set/get round-trip + clear
//   AC3: out-of-bounds get → oob_block (air / bedrock)
//   AC4: OOB set is no-op
//   AC5: index_unchecked / decode_index round-trip (X-major layout)
//   AC6: voxel_fill_box clips and counts
//   AC7: ChunkGrid world get/set across chunk boundaries

#include "test_harness.hpp"

#include "renderer/voxel_volume.hh"

#include <print>

import std;

namespace {

using aura::renderer::BlockId;
using aura::renderer::ChunkGrid;
using aura::renderer::kAir;
using aura::renderer::kBedrock;
using aura::renderer::voxel_fill_box;
using aura::renderer::VoxelVolume;
using aura::test::g_failed;
using aura::test::g_passed;

void ac1_resize_valid() {
    std::println("\n--- AC1: resize / valid / block_count ---");
    VoxelVolume empty;
    CHECK(!empty.valid(), "default invalid");
    CHECK(empty.block_count() == 0, "default empty");

    VoxelVolume v(8, 4, 2, kAir);
    CHECK(v.valid(), "8x4x2 valid");
    CHECK(v.size_x == 8 && v.size_y == 4 && v.size_z == 2, "dims");
    CHECK(v.block_count() == 64, "8*4*2 == 64");

    v.resize(0, 1, 1);
    CHECK(!v.valid(), "zero dim invalid");

    v.resize(128, 128, 128);
    CHECK(v.valid(), "128³ valid");
    CHECK(v.block_count() == 128u * 128u * 128u, "128³ count");
}

void ac2_set_get_clear() {
    std::println("\n--- AC2: set/get/clear ---");
    VoxelVolume v(4, 4, 4, kAir);
    CHECK(v.get(1, 2, 3) == kAir, "initial air");
    CHECK(v.set(1, 2, 3, 42), "set ok");
    CHECK(v.get(1, 2, 3) == 42, "get round-trip");
    v.clear();
    CHECK(v.get(1, 2, 3) == kAir, "clear → air");
    v.fill(7);
    CHECK(v.get(0, 0, 0) == 7 && v.get(3, 3, 3) == 7, "fill all");
}

void ac3_oob_get() {
    std::println("\n--- AC3: OOB get policy ---");
    VoxelVolume v(2, 2, 2, kAir);
    CHECK(v.get(-1, 0, 0) == kAir, "default OOB air");
    CHECK(v.get(0, 0, 99) == kAir, "OOB z air");
    v.oob_block = kBedrock;
    CHECK(v.get(-1, 0, 0) == kBedrock, "OOB → bedrock");
    CHECK(v.get(0, 0, 0) == kAir, "in-bounds still air");
}

void ac4_oob_set_noop() {
    std::println("\n--- AC4: OOB set no-op ---");
    VoxelVolume v(2, 2, 2, kAir);
    CHECK(!v.set(-1, 0, 0, 9), "OOB set false");
    CHECK(!v.set(0, 0, 2, 9), "OOB z set false");
    CHECK(v.get(0, 0, 0) == kAir, "interior unchanged");
}

void ac5_index_layout() {
    std::println("\n--- AC5: X-major index layout ---");
    VoxelVolume v(3, 2, 2, kAir);
    // index = x + 3*(y + 2*z)
    CHECK(v.index_unchecked(0, 0, 0) == 0, "origin 0");
    CHECK(v.index_unchecked(1, 0, 0) == 1, "X+1 contiguous");
    CHECK(v.index_unchecked(2, 0, 0) == 2, "X+2");
    CHECK(v.index_unchecked(0, 1, 0) == 3, "Y+1 stride sx");
    CHECK(v.index_unchecked(0, 0, 1) == 6, "Z+1 stride sx*sy");

    v.set(2, 1, 1, 99);
    CHECK(v.data[v.index_unchecked(2, 1, 1)] == 99, "data[] matches");

    int x = -1, y = -1, z = -1;
    CHECK(v.decode_index(v.index_unchecked(2, 1, 1), x, y, z), "decode ok");
    CHECK(x == 2 && y == 1 && z == 1, "decode round-trip");
    CHECK(!v.decode_index(9999, x, y, z), "decode OOB false");
}

void ac6_fill_box() {
    std::println("\n--- AC6: voxel_fill_box ---");
    VoxelVolume v(8, 8, 8, kAir);
    const auto n = voxel_fill_box(v, 2, 2, 2, 5, 4, 6, 3);
    CHECK(n == 3u * 2u * 4u, "count 3*2*4");
    CHECK(v.get(2, 2, 2) == 3, "corner set");
    CHECK(v.get(4, 3, 5) == 3, "interior set");
    CHECK(v.get(5, 2, 2) == kAir, "half-open x1 excluded");
    CHECK(v.get(1, 2, 2) == kAir, "outside box");
    // Clip negative
    const auto n2 = voxel_fill_box(v, -10, -10, -10, 1, 1, 1, 5);
    CHECK(n2 == 1, "clipped to single origin voxel");
    CHECK(v.get(0, 0, 0) == 5, "origin filled after clip");
}

void ac7_chunk_grid() {
    std::println("\n--- AC7: ChunkGrid across boundaries ---");
    // 2×1×2 chunks of 16 → world 32×16×32
    ChunkGrid g(2, 1, 2, kAir);
    CHECK(g.valid(), "chunk grid valid");
    CHECK(g.size_x() == 32 && g.size_y() == 16 && g.size_z() == 32, "world size");
    CHECK(g.kChunkSize == 16, "chunk size 16");

    CHECK(g.set(0, 0, 0, 11), "set chunk0 origin");
    CHECK(g.get(0, 0, 0) == 11, "get origin");

    // Boundary: last block of chunk (0,0,0) and first of neighbor +X
    CHECK(g.set(15, 0, 0, 12), "chunk edge");
    CHECK(g.set(16, 0, 0, 13), "next chunk");
    CHECK(g.get(15, 0, 0) == 12, "edge read");
    CHECK(g.get(16, 0, 0) == 13, "neighbor read");

    // +Z neighbor
    CHECK(g.set(0, 0, 16, 14), "chunk +Z");
    CHECK(g.get(0, 0, 16) == 14, "+Z read");

    CHECK(g.get(-1, 0, 0) == kAir, "OOB air");
    g.oob_block = kBedrock;
    CHECK(g.get(100, 0, 0) == kBedrock, "OOB bedrock");
    CHECK(!g.set(-1, 0, 0, 1), "OOB set false");
}

} // namespace

int main() {
    std::println("=== test_voxel_volume (#1982 / epic #1979) ===");
    ac1_resize_valid();
    ac2_set_get_clear();
    ac3_oob_get();
    ac4_oob_set_noop();
    ac5_index_layout();
    ac6_fill_box();
    ac7_chunk_grid();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
