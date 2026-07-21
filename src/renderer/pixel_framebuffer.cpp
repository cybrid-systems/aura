// pixel_framebuffer.cpp — Issue #1980: half-block pixel framebuffer layer.

#include "renderer/pixel_framebuffer.hh"

#include <algorithm>
#include <cstring>

namespace aura::renderer {
namespace {

    PixelFramebufferCounters g_pixel_counters{};

    [[nodiscard]] TermCell* cell_at(PixelFramebuffer& pf, int cell_x, int cell_y) noexcept {
        if (!pf.cells.valid() || cell_x < 0 || cell_y < 0 || cell_x >= pf.cells.width ||
            cell_y >= pf.cells.height)
            return nullptr;
        const auto idx =
            static_cast<std::size_t>(cell_y) * static_cast<std::size_t>(pf.cells.width) +
            static_cast<std::size_t>(cell_x);
        return &pf.cells.cells[idx];
    }

    [[nodiscard]] const TermCell* cell_at_c(const PixelFramebuffer& pf, int cell_x,
                                            int cell_y) noexcept {
        if (!pf.cells.valid() || cell_x < 0 || cell_y < 0 || cell_x >= pf.cells.width ||
            cell_y >= pf.cells.height)
            return nullptr;
        const auto idx =
            static_cast<std::size_t>(cell_y) * static_cast<std::size_t>(pf.cells.width) +
            static_cast<std::size_t>(cell_x);
        return &pf.cells.cells[idx];
    }

    // Ensure cell is a half-block glyph in RGB mode; preserve existing halves when converting.
    void ensure_half_block(TermCell& c) noexcept {
        if (c.ch == kHalfBlockGlyph && c.mode == 1)
            return;
        // Convert: if previously RGB, keep fg/bg as upper/lower; palette → black halves.
        if (c.mode != 1) {
            c.fg_r = c.fg_g = c.fg_b = 0;
            c.bg_r = c.bg_g = c.bg_b = 0;
        }
        c.ch = kHalfBlockGlyph;
        c.mode = 1;
    }

    void apply_upper(TermCell& c, Color32 col) noexcept {
        ensure_half_block(c);
        c.fg_r = col.r;
        c.fg_g = col.g;
        c.fg_b = col.b;
    }

    void apply_lower(TermCell& c, Color32 col) noexcept {
        ensure_half_block(c);
        c.bg_r = col.r;
        c.bg_g = col.g;
        c.bg_b = col.b;
    }

    void note_pixel_write(PixelFramebuffer& pf, int cell_x, int cell_y) noexcept {
        ++pf.pixels_written;
        ++g_pixel_counters.pixels_written_total;
        if (pf.dirty)
            pf.dirty->mark_dirty(static_cast<std::uint32_t>(cell_x),
                                 static_cast<std::uint32_t>(cell_y));
    }

    void note_cell_touch(PixelFramebuffer& pf) noexcept {
        ++pf.half_block_cells_touched;
        ++g_pixel_counters.half_block_cells_touched_total;
    }

} // namespace

PixelFramebuffer pixel_framebuffer_from_soa(FramebufferSoA fb, DirtyRegion& dirty) noexcept {
    PixelFramebuffer pf;
    pf.cells = fb;
    pf.dirty = &dirty;
    if (fb.valid()) {
        pf.pixel_width = fb.width;
        pf.pixel_height = fb.height * 2;
    }
    return pf;
}

void pixel_clear(PixelFramebuffer& pf, Color32 c) {
    ++g_pixel_counters.clear_calls;
    if (!pf.valid())
        return;
    TermCell cell{};
    cell.ch = kHalfBlockGlyph;
    cell.mode = 1;
    cell.fg_r = cell.bg_r = c.r;
    cell.fg_g = cell.bg_g = c.g;
    cell.fg_b = cell.bg_b = c.b;
    const auto n = pf.cells.cell_count();
    for (std::size_t i = 0; i < n; ++i)
        pf.cells.cells[i] = cell;
    const auto pixels =
        static_cast<std::uint64_t>(pf.pixel_width) * static_cast<std::uint64_t>(pf.pixel_height);
    pf.pixels_written += pixels;
    g_pixel_counters.pixels_written_total += pixels;
    pf.half_block_cells_touched += n;
    g_pixel_counters.half_block_cells_touched_total += n;
    pf.dirty->mark_all_dirty(static_cast<std::uint32_t>(pf.cells.width),
                             static_cast<std::uint32_t>(pf.cells.height));
}

bool pixel_set(PixelFramebuffer& pf, int x, int y, Color32 c) {
    if (!pf.valid() || x < 0 || y < 0 || x >= pf.pixel_width || y >= pf.pixel_height)
        return false;
    const int cell_x = x;
    const int cell_y = y / 2;
    const bool upper = (y % 2) == 0;
    TermCell* cell = cell_at(pf, cell_x, cell_y);
    if (!cell)
        return false;
    const bool was_half = (cell->ch == kHalfBlockGlyph && cell->mode == 1);
    if (upper)
        apply_upper(*cell, c);
    else
        apply_lower(*cell, c);
    note_pixel_write(pf, cell_x, cell_y);
    if (!was_half)
        note_cell_touch(pf);
    return true;
}

Color32 pixel_get(const PixelFramebuffer& pf, int x, int y) noexcept {
    if (!pf.cells.valid() || x < 0 || y < 0 || x >= pf.pixel_width || y >= pf.pixel_height)
        return Color32{};
    const int cell_x = x;
    const int cell_y = y / 2;
    const bool upper = (y % 2) == 0;
    const TermCell* cell = cell_at_c(pf, cell_x, cell_y);
    if (!cell)
        return Color32{};
    if (cell->ch == kHalfBlockGlyph && cell->mode == 1) {
        return upper ? Color32::from_rgb(cell->fg_r, cell->fg_g, cell->fg_b)
                     : Color32::from_rgb(cell->bg_r, cell->bg_g, cell->bg_b);
    }
    // Non half-block: approximate with fg (upper) / bg (lower) in RGB mode only.
    if (cell->mode == 1) {
        return upper ? Color32::from_rgb(cell->fg_r, cell->fg_g, cell->fg_b)
                     : Color32::from_rgb(cell->bg_r, cell->bg_g, cell->bg_b);
    }
    return Color32{};
}

void pixel_fill_rect(PixelFramebuffer& pf, int x, int y, int w, int h, Color32 c) {
    if (!pf.valid() || w <= 0 || h <= 0)
        return;
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(pf.pixel_width, x + w);
    const int y1 = std::min(pf.pixel_height, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px)
            (void)pixel_set(pf, px, py, c);
    }
}

void pixel_blit_rgb888(PixelFramebuffer& pf, const std::uint8_t* rgb, int src_w, int src_h,
                       int stride_bytes) {
    if (!pf.valid() || !rgb || src_w <= 0 || src_h <= 0)
        return;
    if (stride_bytes <= 0)
        stride_bytes = src_w * 3;
    const int w = std::min(pf.pixel_width, src_w);
    const int h = std::min(pf.pixel_height, src_h);
    for (int py = 0; py < h; ++py) {
        const std::uint8_t* row =
            rgb + static_cast<std::size_t>(py) * static_cast<std::size_t>(stride_bytes);
        for (int px = 0; px < w; ++px) {
            const auto* p = row + static_cast<std::size_t>(px) * 3u;
            (void)pixel_set(pf, px, py, Color32::from_rgb(p[0], p[1], p[2]));
        }
    }
}

void pixel_blit_u32(PixelFramebuffer& pf, const std::uint32_t* pixels, int src_w, int src_h,
                    int stride_px) {
    if (!pf.valid() || !pixels || src_w <= 0 || src_h <= 0)
        return;
    if (stride_px <= 0)
        stride_px = src_w;
    const int w = std::min(pf.pixel_width, src_w);
    const int h = std::min(pf.pixel_height, src_h);
    for (int py = 0; py < h; ++py) {
        const std::uint32_t* row =
            pixels + static_cast<std::size_t>(py) * static_cast<std::size_t>(stride_px);
        for (int px = 0; px < w; ++px)
            (void)pixel_set(pf, px, py, Color32::from_u32(row[px]));
    }
}

std::int64_t pixel_present(PixelFramebuffer& pf, int fd) {
    ++g_pixel_counters.present_calls;
    if (!pf.valid())
        return -1;
    return present_batch(pf.cells, *pf.dirty, fd);
}

std::int64_t pixel_present_to_string(PixelFramebuffer& pf, std::string& out) {
    ++g_pixel_counters.present_calls;
    if (!pf.valid())
        return -1;
    return present_batch_to_string(pf.cells, *pf.dirty, out);
}

PixelFramebufferCounters& pixel_framebuffer_counters() noexcept {
    return g_pixel_counters;
}

void reset_pixel_framebuffer_counters_for_test() noexcept {
    g_pixel_counters = {};
}

} // namespace aura::renderer
