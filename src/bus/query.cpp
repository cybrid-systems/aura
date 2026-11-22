// ──────────────────────────────────────────────────────────────
//  query.cpp — reader-side aggregations over a committed image (#5125)
// ──────────────────────────────────────────────────────────────
#include "bus/query.hh"

#include "bus/image.hh"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace aura::bus {
namespace {

    template <typename T> T load_at(std::span<const std::byte> image, std::uint64_t off) {
        T v{};
        std::memcpy(&v, image.data() + off, sizeof(T));
        return v;
    }

    bool section_records(std::span<const std::byte> image, SectionKind kind, SectionEntry* e) {
        if (!find_section(image, kind, e))
            return false;
        return e->element_size != 0 && e->offset <= image.size() &&
               e->length <= image.size() - e->offset && e->length % e->element_size == 0;
    }

} // namespace

bool cct_hotspots(std::span<const std::byte> image, std::size_t top, std::vector<Hotspot>* out,
                  std::string* why) {
    if (out == nullptr) {
        if (why)
            *why = "null hotspot output";
        return false;
    }
    out->clear();
    SectionEntry e{};
    if (!section_records(image, SectionKind::Cct, &e)) {
        if (why)
            *why = "CCT section is missing";
        return false;
    }
    std::unordered_map<std::uint64_t, Hotspot> by_mid;
    by_mid.reserve(static_cast<std::size_t>(e.count));
    for (std::uint64_t i = 0; i < e.count; ++i) {
        const CctRecord c = load_at<CctRecord>(image, e.offset + i * e.element_size);
        Hotspot& h = by_mid[c.mid_lo];
        h.mid = c.mid_lo;
        h.samples += c.samples;
        h.self_ns += c.self_ns;
        if (h.node == kImageNoIndex)
            h.node = c.node;
    }
    out->reserve(by_mid.size());
    for (auto& [mid, h] : by_mid) {
        (void)mid;
        out->push_back(h);
    }
    std::sort(out->begin(), out->end(), [](const Hotspot& a, const Hotspot& b) {
        if (a.self_ns != b.self_ns)
            return a.self_ns > b.self_ns;
        if (a.samples != b.samples)
            return a.samples > b.samples;
        return a.mid < b.mid;
    });
    if (top != 0 && out->size() > top)
        out->resize(top);
    return true;
}

bool worldline_diff(std::span<const std::byte> image, std::uint64_t id_a, std::uint64_t id_b,
                    WorldlineDiff* out, std::string* why) {
    const auto fail = [&](const char* text) {
        if (why)
            *why = text;
        return false;
    };
    if (out == nullptr)
        return fail("null diff output");
    SectionEntry wl{};
    if (!section_records(image, SectionKind::Worldlines, &wl))
        return fail("worldline section is missing");
    const WorldlineRecord* a = nullptr;
    const WorldlineRecord* b = nullptr;
    WorldlineRecord ra{};
    WorldlineRecord rb{};
    for (std::uint64_t i = 0; i < wl.count; ++i) {
        WorldlineRecord w{};
        std::memcpy(&w, image.data() + wl.offset + i * wl.element_size,
                    std::min<std::uint64_t>(wl.element_size, sizeof(w)));
        if (a == nullptr && w.id == id_a) {
            ra = w;
            a = &ra;
        }
        if (b == nullptr && w.id == id_b) {
            rb = w;
            b = &rb;
        }
        if (a != nullptr && b != nullptr)
            break;
    }
    if (a == nullptr)
        return fail("first worldline id is absent");
    if (b == nullptr)
        return fail("second worldline id is absent");

    out->state_a = a->state;
    out->state_b = b->state;
    out->constraint_a = a->constraint_result;
    out->constraint_b = b->constraint_result;
    out->metrics_a = a->metrics_off;
    out->metrics_b = b->metrics_off;
    out->rows.clear();

    SectionEntry patches{};
    const bool have_patches = section_records(image, SectionKind::Patches, &patches);
    using PatchValue = std::pair<std::uint32_t, std::uint64_t>;
    auto collect = [&](const WorldlineRecord& w,
                       std::unordered_map<std::uint64_t, PatchValue>* into) {
        if (!have_patches || w.patch_len == 0)
            return;
        if (w.patch_off > patches.length || w.patch_len > patches.length - w.patch_off)
            return;
        const std::uint64_t count = w.patch_len / patches.element_size;
        for (std::uint64_t i = 0; i < count; ++i) {
            const PatchRecord p = load_at<PatchRecord>(image, patches.offset + w.patch_off +
                                                                  i * patches.element_size);
            (*into)[p.node] = {p.field_offset, p.new_value};
        }
    };
    std::unordered_map<std::uint64_t, PatchValue> pa;
    std::unordered_map<std::uint64_t, PatchValue> pb;
    collect(*a, &pa);
    collect(*b, &pb);

    auto append = [&](std::uint64_t node, const PatchValue* va, const PatchValue* vb) {
        PatchDiff d{};
        d.node = node;
        d.in_a = va != nullptr;
        d.in_b = vb != nullptr;
        if (va != nullptr) {
            d.field_a = va->first;
            d.value_a = va->second;
        }
        if (vb != nullptr) {
            d.field_b = vb->first;
            d.value_b = vb->second;
        }
        out->rows.push_back(d);
    };
    for (const auto& [node, va] : pa) {
        const auto it = pb.find(node);
        if (it == pb.end()) {
            append(node, &va, nullptr);
        } else if (it->second.first != va.first || it->second.second != va.second) {
            append(node, &va, &it->second);
        }
    }
    for (const auto& [node, vb] : pb) {
        if (pa.find(node) == pa.end())
            append(node, nullptr, &vb);
    }
    std::sort(out->rows.begin(), out->rows.end(),
              [](const PatchDiff& x, const PatchDiff& y) { return x.node < y.node; });
    return true;
}

} // namespace aura::bus
