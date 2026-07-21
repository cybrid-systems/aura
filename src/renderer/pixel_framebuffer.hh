// pixel_framebuffer.hh — Issue #1980 / Epic #1979: half-block pixel framebuffer.
//
// Thin pixel abstraction on top of TermCell grids for software 3D raycasters.
// Each terminal cell maps to **two vertical pixels** via Unicode U+2580 ▀:
//   - even pixel_y (upper) → TermCell foreground (RGB mode)
//   - odd  pixel_y (lower) → TermCell background (RGB mode)
//
// Coordinate system: origin top-left, y increases downward (terminal match).
// Prefer zero-copy: PixelFramebuffer views existing FramebufferSoA storage.

#ifndef AURA_RENDERER_PIXEL_FRAMEBUFFER_HH
#define AURA_RENDERER_PIXEL_FRAMEBUFFER_HH

#include "renderer/render_primitives.hh"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aura::renderer {

inline constexpr int kPixelFramebufferIssue = 1980;
inline constexpr int kPixelFramebufferEpic = 1979;

// U+2580 UPPER HALF BLOCK — upper half = fg, lower half = bg.
inline constexpr std::uint32_t kHalfBlockGlyph = 0x2580u;

// 24-bit RGB color (alpha unused).
struct Color32 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    [[nodiscard]] static constexpr Color32 from_rgb(std::uint8_t rr, std::uint8_t gg,
                                                    std::uint8_t bb) noexcept {
        return Color32{rr, gg, bb};
    }
    // Pack 0x00RRGGBB (high byte ignored).
    [[nodiscard]] static constexpr Color32 from_u32(std::uint32_t rgb) noexcept {
        return Color32{static_cast<std::uint8_t>((rgb >> 16) & 0xFFu),
                       static_cast<std::uint8_t>((rgb >> 8) & 0xFFu),
                       static_cast<std::uint8_t>(rgb & 0xFFu)};
    }
    [[nodiscard]] constexpr std::uint32_t to_u32() const noexcept {
        return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) |
               static_cast<std::uint32_t>(b);
    }
    [[nodiscard]] constexpr bool operator==(const Color32& o) const noexcept {
        return r == o.r && g == o.g && b == o.b;
    }
};

// Pixel view over a cell framebuffer. pixel_height must be even (= cell_rows * 2).
struct PixelFramebuffer {
    int pixel_width = 0;  // = term cols
    int pixel_height = 0; // = term rows * 2
    FramebufferSoA cells{};
    DirtyRegion* dirty = nullptr; // external dirty tracker; required for present

    // Local counters (also aggregated into process-wide counters below).
    std::uint64_t pixels_written = 0;
    std::uint64_t half_block_cells_touched = 0;

    [[nodiscard]] bool valid() const noexcept {
        return cells.valid() && pixel_width > 0 && pixel_height > 0 && (pixel_height % 2) == 0 &&
               cells.width == pixel_width && cells.height == pixel_height / 2 && dirty != nullptr;
    }

    [[nodiscard]] int cell_cols() const noexcept { return pixel_width; }
    [[nodiscard]] int cell_rows() const noexcept { return pixel_height / 2; }
};

// Build a pixel view over an existing SoA framebuffer + dirty region.
// pixel_height = fb.height * 2, pixel_width = fb.width.
[[nodiscard]] PixelFramebuffer pixel_framebuffer_from_soa(FramebufferSoA fb,
                                                          DirtyRegion& dirty) noexcept;

// Convenience: view owned storage.
[[nodiscard]] inline PixelFramebuffer
pixel_framebuffer_from_owned(FramebufferOwned& owned) noexcept {
    return pixel_framebuffer_from_soa(owned.view(), owned.dirty);
}

// Fill all pixels with color (packs half-blocks into every cell).
void pixel_clear(PixelFramebuffer& pf, Color32 c);

// Set a single pixel. Returns false if out of bounds / invalid.
// Marks the owning cell dirty (cell granularity).
[[nodiscard]] bool pixel_set(PixelFramebuffer& pf, int x, int y, Color32 c);

// Read back a pixel (from packed half-block cell). Returns black if OOB.
[[nodiscard]] Color32 pixel_get(const PixelFramebuffer& pf, int x, int y) noexcept;

// Axis-aligned fill [x, x+w) x [y, y+h) in pixel coords.
void pixel_fill_rect(PixelFramebuffer& pf, int x, int y, int w, int h, Color32 c);

// Blit a full RGB888 frame (row-major, 3 bytes/pixel). stride_bytes=0 → width*3.
// Clips to framebuffer size.
void pixel_blit_rgb888(PixelFramebuffer& pf, const std::uint8_t* rgb, int src_w, int src_h,
                       int stride_bytes = 0);

// Blit 0x00RRGGBB row-major u32 pixels. stride_px=0 → src_w.
void pixel_blit_u32(PixelFramebuffer& pf, const std::uint32_t* pixels, int src_w, int src_h,
                    int stride_px = 0);

// Pack is already live in TermCell storage; present via present_batch.
// Returns bytes written / 0 skip / -1 invalid (same as present_batch).
[[nodiscard]] std::int64_t pixel_present(PixelFramebuffer& pf, int fd = 1);

// Headless present into string (for unit tests).
[[nodiscard]] std::int64_t pixel_present_to_string(PixelFramebuffer& pf, std::string& out);

// Process-wide counters for observability / tests.
struct PixelFramebufferCounters {
    std::uint64_t pixels_written_total = 0;
    std::uint64_t half_block_cells_touched_total = 0;
    std::uint64_t present_calls = 0;
    std::uint64_t clear_calls = 0;
};

[[nodiscard]] PixelFramebufferCounters& pixel_framebuffer_counters() noexcept;
void reset_pixel_framebuffer_counters_for_test() noexcept;

} // namespace aura::renderer

#endif // AURA_RENDERER_PIXEL_FRAMEBUFFER_HH
