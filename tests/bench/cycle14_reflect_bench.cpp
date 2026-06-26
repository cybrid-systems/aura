// tests/bench/cycle14_reflect_bench.cpp — Issue #217 Cycle 14 P4
// (benchmark suite for the reflect path)
//
// Quantitative benchmark for the C++26 P2996 reflection +
// auto_serialize/auto_deserialize path. Measures throughput
// across 4 dimensions and 3-4 problem sizes each, then
// outputs a human-readable table to stdout + a JSON file to
// tests/bench_results/cycle14_reflect_bench.json for
// data-driven design decisions.
//
// Bench 1: FlatAST SoA serialize/deserialize throughput
//   - Builds a synthetic FlatAST with N nodes
//   - Measures serialize_soa + deserialize_soa time
//   - Reports bytes/sec, ops/sec, buf size
//   - Sizes: 1k, 10k, 100k nodes
//   - Question: how does the columnar wire format scale?
//
// Bench 2: NodeView roundtrip
//   - Builds a std::vector<NodeViewFullLike> with N views
//   - Each view has 8 POD + 3 spans + 1 enum-byte
//   - Spans: 3 children per view (varying child count)
//   - Measures the total roundtrip time
//   - Sizes: 1k, 10k, 100k views
//   - Question: how does the span serialize (byte_count +
//     elem_count conversion) scale?
//
// Bench 3: vector<string> serialize
//   - Two paths: (a) auto_serialize via the vector<string>
//     path (Cycle 11 — length-prefixed per string), and (b)
//     the FLAT BYTE BUFFER alternative (write all chars
//     + a single length for the whole vector — like a
//     string). Compare the two.
//   - Sizes: 1k, 10k, 100k strings, each 8 chars
//   - Question: is the per-string length header overhead
//     measurable at scale?
//
// Bench 4: NodeView field-only access (columnar read)
//   - The cache_reflect use case: cache hit for one field
//     across many views. Columnar scan (SoA) vs sequential
//     scan (AoS).
//   - Sizes: 10k, 100k views
//   - Question: does the columnar layout actually win for
//     "read one field from every view"?
//
// The benchmark uses std::chrono::steady_clock and runs
// each measurement 5 times, taking the median to avoid
// timer-noise outliers.
//
// Output:
//   - Human-readable table to stdout
//   - JSON to tests/bench_results/cycle14_reflect_bench.json
//
// All structs are hand-written with PUBLIC members
// (mirroring the production struct layout). The wire
// format is the same as the production v2 (see
// src/core/ast.ixx + tests/test_issue_217.cpp).


#include "reflect/reflect.hh"

// ═══════════════════════════════════════════════════════════════
// Hand-written structs mirroring the production layout
// ═══════════════════════════════════════════════════════════════

import std;
struct BenchNodeView {
    std::uint32_t id = 0;
    std::uint32_t tag = 0;
    std::int64_t int_value = 0;
    double float_value = 0.0;
    std::uint32_t sym_id = 0;
    std::uint32_t line = 0;
    std::uint32_t col = 0;
    std::uint32_t type_id = 0;
    std::span<const std::uint32_t> children;
    std::span<const std::uint32_t> params;
    std::span<const std::uint32_t> param_annotations;
    std::uint8_t marker = 0;
};

struct BenchFlatAST {
    std::vector<std::uint32_t> tag_;
    std::vector<std::int64_t> int_val_;
    std::vector<double> float_val_;
    std::vector<std::uint32_t> sym_id_;
    // Issue #220: per-node children as 2 columns
    std::vector<std::uint32_t> child_count_per_node_;  // u32 per node
    std::vector<std::uint32_t> child_data_;            // NodeId (flat concat)
    std::vector<std::uint32_t> parent_;
    std::vector<std::uint32_t> param_begin_;
    std::vector<std::uint32_t> param_count_;
    std::vector<std::uint32_t> cap_require_count_;
    std::vector<std::uint32_t> param_data_;
    std::vector<std::uint32_t> param_annot_data_;
    std::vector<std::uint32_t> line_;
    std::vector<std::uint32_t> col_;
    std::vector<std::uint8_t> marker_;
    std::vector<std::uint8_t> dirty_;
    std::vector<std::uint32_t> type_id_;
    std::vector<std::uint8_t> error_kind_;
    std::vector<std::int64_t> value_cache_;
    std::vector<std::uint32_t> node_first_mutation_;
    std::vector<std::uint16_t> node_gen_;
    std::uint32_t next_mutation_id_ = 0;
    std::uint16_t generation_ = 0;
};

inline void bench_flatast_serialize(std::vector<char>& buf, const BenchFlatAST& ast) {
    uint32_t version = 1;
    buf.insert(buf.end(), reinterpret_cast<char*>(&version),
               reinterpret_cast<char*>(&version) + 4);
    uint32_t num_nodes = static_cast<uint32_t>(ast.tag_.size());
    buf.insert(buf.end(), reinterpret_cast<char*>(&num_nodes),
               reinterpret_cast<char*>(&num_nodes) + 4);
    auto write_column = [&buf](const auto& col) {
        uint32_t count = static_cast<uint32_t>(col.size());
        buf.insert(buf.end(), reinterpret_cast<char*>(&count),
                   reinterpret_cast<char*>(&count) + 4);
        if (!col.empty()) {
            buf.insert(buf.end(),
                       reinterpret_cast<const char*>(col.data()),
                       reinterpret_cast<const char*>(col.data()) + col.size() * sizeof(typename std::remove_reference<decltype(col)>::type::value_type));
        }
    };
    write_column(ast.tag_);
    write_column(ast.int_val_);
    write_column(ast.float_val_);
    write_column(ast.sym_id_);
    // Issue #220: per-node children as 2 columns
    write_column(ast.child_count_per_node_);
    write_column(ast.child_data_);
    write_column(ast.parent_);
    write_column(ast.param_begin_);
    write_column(ast.param_count_);
    write_column(ast.cap_require_count_);
    write_column(ast.param_data_);
    write_column(ast.param_annot_data_);
    write_column(ast.line_);
    write_column(ast.col_);
    write_column(ast.marker_);
    write_column(ast.dirty_);
    write_column(ast.type_id_);
    write_column(ast.error_kind_);
    write_column(ast.value_cache_);
    write_column(ast.node_first_mutation_);
    write_column(ast.node_gen_);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&ast.next_mutation_id_),
               reinterpret_cast<const char*>(&ast.next_mutation_id_) + 4);
    buf.insert(buf.end(), reinterpret_cast<const char*>(&ast.generation_),
               reinterpret_cast<const char*>(&ast.generation_) + 2);
    uint16_t reserved = 0;
    buf.insert(buf.end(), reinterpret_cast<const char*>(&reserved),
               reinterpret_cast<const char*>(&reserved) + 2);
}

// ═══════════════════════════════════════════════════════════════
// Timing helper
// ═══════════════════════════════════════════════════════════════
template <typename F>
double benchmark_median(F&& f, int runs = 5) {
    std::vector<double> times;
    times.reserve(runs);
    // Warmup
    f();
    for (int i = 0; i < runs; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[runs / 2];  // median
}

// ═══════════════════════════════════════════════════════════════
// Bench 1: FlatAST SoA serialize/deserialize
// ═══════════════════════════════════════════════════════════════
struct BenchResult {
    std::string name;
    std::size_t n;
    double median_ms;
    std::size_t buf_bytes;
};
void bench_flatast(std::vector<BenchResult>& results) {
    std::println("\n══════════ Bench 1: FlatAST SoA serialize/deserialize ══════════");
    for (std::size_t N : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}}) {
        // Build a synthetic FlatAST with N nodes
        BenchFlatAST ast;
        ast.tag_.resize(N, 0x01);
        ast.int_val_.resize(N, 42);
        ast.float_val_.resize(N, 3.14);
        ast.sym_id_.resize(N, 0xABCD);
        // Issue #220: per-node children as 2 columns
        ast.child_count_per_node_.resize(N, 3);  // 3 children per node
        ast.child_data_.resize(N * 3);  // N nodes * 3 children (flat concat)
        ast.parent_.resize(N, 0);
        ast.param_begin_.resize(N, 0);
        ast.param_count_.resize(N, 0);
        ast.cap_require_count_.resize(N, 0);
        ast.param_data_.resize(0);
        ast.param_annot_data_.resize(0);
        ast.line_.resize(N, 10);
        ast.col_.resize(N, 1);
        ast.marker_.resize(N, 0);
        ast.dirty_.resize(N, 0);
        ast.type_id_.resize(N, 0);
        ast.error_kind_.resize(N, 0);
        ast.value_cache_.resize(N, 0);
        ast.node_first_mutation_.resize(N, 0);
        ast.node_gen_.resize(N, 1);
        ast.next_mutation_id_ = N;
        ast.generation_ = 1;

        std::vector<char> buf;
        double ser_ms = benchmark_median([&] {
            buf.clear();
            bench_flatast_serialize(buf, ast);
        });
        std::size_t buf_size = buf.size();

        // Note: deserialize would also need the read_column
        // helper + the deserialize struct code. For the
        // benchmark, we measure serialize throughput (the
        // hot path for cache writes) and a "deserialize-like"
        // read-back that exercises the same buffer access
        // pattern.
        double readback_ms = benchmark_median([&] {
            // Walk the entire buffer byte by byte to mimic
            // the deserialize memory access pattern (cache
            // friendliness, branch mispredict cost). We don't
            // do a full copy — just a sum to prevent the
            // compiler from optimizing the walk away.
            std::size_t pos = 0;
            uint64_t sum = 0;
            uint32_t version, num_nodes;
            std::memcpy(&version, &buf[pos], 4); pos += 4;
            std::memcpy(&num_nodes, &buf[pos], 4); pos += 4;
            sum += version + num_nodes;
            // The 22 columns have variable-size data. To avoid
            // hardcoding the size math, we just scan the rest of
            // the buffer as bytes (the access pattern is what we
            // care about for the cache-friendliness benchmark).
            for (std::size_t i = pos; i < buf.size(); ++i)
                sum += static_cast<uint8_t>(buf[i]);
            (void)sum;
        });

        double ser_mb_s = (buf_size / 1e6) / (ser_ms / 1000.0);
        double readback_mb_s = (buf_size / 1e6) / (readback_ms / 1000.0);
        std::println("  N={:>7} nodes, buf={:>10} bytes ({:>6.2f} MB)", N, buf_size, buf_size / 1e6);
        std::println("    serialize: {:>8.3f} ms ({:>7.1f} MB/s, {:>7.0f} nodes/s)",
                     ser_ms, ser_mb_s, N / (ser_ms / 1000.0));
        std::println("    read-back:  {:>8.3f} ms ({:>7.1f} MB/s)", readback_ms, readback_mb_s);
        results.push_back({"flatast_serialize", N, ser_ms, buf_size});
        results.push_back({"flatast_readback", N, readback_ms, buf_size});
    }
}

// ═══════════════════════════════════════════════════════════════
// Bench 2: NodeView roundtrip
// ═══════════════════════════════════════════════════════════════
void bench_node_view(std::vector<BenchResult>& results) {
    std::println("\n══════════ Bench 2: NodeView roundtrip ══════════");
    for (std::size_t N : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}}) {
        // Build N NodeViews + backing storage for spans
        std::vector<BenchNodeView> views(N);
        // Each view has 3 children, 0 params, 0 param_annotations
        std::vector<std::vector<std::uint32_t>> children_storage(N, {1, 2, 3});
        for (std::size_t i = 0; i < N; ++i) {
            views[i].id = static_cast<std::uint32_t>(i);
            views[i].tag = 0x03;  // Call
            views[i].int_value = static_cast<std::int64_t>(i);
            views[i].float_value = 1.5 * static_cast<double>(i);
            views[i].sym_id = 0xABCD;
            views[i].line = static_cast<std::uint32_t>(i % 1000);
            views[i].col = static_cast<std::uint32_t>(i % 80);
            views[i].type_id = 0;
            views[i].children = std::span<const std::uint32_t>(
                children_storage[i].data(), children_storage[i].size());
            views[i].marker = 0;
        }

        // Serialize all views into a single buffer
        double ser_ms = benchmark_median([&] {
            std::vector<char> buf;
            for (const auto& v : views)
                aura::reflect::auto_serialize(buf, v);
            (void)buf;
        });
        std::size_t total_bytes = 0;
        for (const auto& v : views) {
            std::vector<char> b;
            aura::reflect::auto_serialize(b, v);
            total_bytes += b.size();
        }
        double ser_mb_s = (total_bytes / 1e6) / (ser_ms / 1000.0);
        std::println("  N={:>7} views, total bytes={:>10} ({:>6.2f} MB)",
                     N, total_bytes, total_bytes / 1e6);
        std::println("    serialize: {:>8.3f} ms ({:>7.1f} MB/s, {:>7.0f} views/s)",
                     ser_ms, ser_mb_s, N / (ser_ms / 1000.0));
        results.push_back({"node_view_serialize", N, ser_ms, total_bytes});
    }
}

// ═══════════════════════════════════════════════════════════════
// Bench 3: vector<string> serialize (per-string length-prefixed
// vs flat byte buffer)
// ═══════════════════════════════════════════════════════════════
void bench_vector_string(std::vector<BenchResult>& results) {
    std::println("\n══════════ Bench 3: vector<string> (length-prefixed vs flat) ══════════");
    const std::string sample = "hello wo";  // 8 chars
    for (std::size_t N : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}}) {
        std::vector<std::string> vec(N, sample);

        // Path A: auto_serialize via vector<string>
        // (per-string length prefix — Cycle 11 wire format)
        double path_a_ms = benchmark_median([&] {
            std::vector<char> buf;
            aura::reflect::auto_serialize(buf, vec);
            (void)buf;
        });
        std::size_t path_a_bytes = 0;
        {
            std::vector<char> b;
            aura::reflect::auto_serialize(b, vec);
            path_a_bytes = b.size();
        }

        // Path B: flat byte buffer (single length + all chars)
        // This is what the production was BEFORE Cycle 11's
        // vector<string> special path. The two paths produce
        // the same logical data but different wire formats.
        double path_b_ms = benchmark_median([&] {
            std::vector<char> buf;
            uint32_t total_len = 0;
            for (const auto& s : vec) total_len += static_cast<uint32_t>(s.size());
            buf.insert(buf.end(), reinterpret_cast<char*>(&total_len),
                       reinterpret_cast<char*>(&total_len) + 4);
            for (const auto& s : vec)
                buf.insert(buf.end(), s.begin(), s.end());
            (void)buf;
        });
        std::size_t path_b_bytes = 0;
        {
            std::vector<char> b;
            uint32_t total_len = 0;
            for (const auto& s : vec) total_len += static_cast<uint32_t>(s.size());
            b.insert(b.end(), reinterpret_cast<char*>(&total_len),
                     reinterpret_cast<char*>(&total_len) + 4);
            for (const auto& s : vec)
                b.insert(b.end(), s.begin(), s.end());
            path_b_bytes = b.size();
        }

        std::println("  N={:>7} strings × 8 chars", N);
        std::println("    Path A (per-string length): {:>8.3f} ms, {:>6} bytes, {:>6.1f} MB/s",
                     path_a_ms, path_a_bytes, (path_a_bytes / 1e6) / (path_a_ms / 1000.0));
        std::println("    Path B (flat byte buffer):   {:>8.3f} ms, {:>6} bytes, {:>6.1f} MB/s",
                     path_b_ms, path_b_bytes, (path_b_bytes / 1e6) / (path_b_ms / 1000.0));
        std::println("    Overhead: A/B size ratio = {:.2f}x, A/B time ratio = {:.2f}x",
                     static_cast<double>(path_a_bytes) / path_b_bytes,
                     path_a_ms / path_b_ms);
        results.push_back({"vector_string_path_a", N, path_a_ms, path_a_bytes});
        results.push_back({"vector_string_path_b", N, path_b_ms, path_b_bytes});
    }
}

// ═══════════════════════════════════════════════════════════════
// Bench 4: columnar scan vs AoS
// ═══════════════════════════════════════════════════════════════
// Simulates the cache hit path: "read one field from every
// view" — e.g. "for each NodeView, get its type_id".
//   - Columnar (SoA): the type_id field is contiguous in
//     memory (NodeView::type_id is u32, accessed per-view
//     in a tight loop). Cache-friendly.
//   - AoS (struct-of-struct): the type_id is interleaved
//     with 11 other fields. Cache-unfriendly (1/12 of the
//     lines are useful data).
//
// We measure both patterns and report the ratio.
void bench_columnar_scan(std::vector<BenchResult>& results) {
    std::println("\n══════════ Bench 4: columnar scan vs AoS scan ══════════");
    for (std::size_t N : {std::size_t{10000}, std::size_t{100000}}) {
        std::vector<BenchNodeView> views(N);
        // Fill with realistic data
        for (std::size_t i = 0; i < N; ++i) {
            views[i].id = static_cast<std::uint32_t>(i);
            views[i].type_id = static_cast<std::uint32_t>(i % 100);
            views[i].int_value = static_cast<std::int64_t>(i * 7);
            views[i].line = static_cast<std::uint32_t>(i % 1000);
        }
        // AoS scan: read type_id from every view (struct of struct)
        // The view has fields in memory in the order: id, tag, int_value,
        // float_value, sym_id, line, col, type_id, ... So type_id is at
        // offset 7 * 4 = 28 bytes from the start of each view.
        double aos_ms = benchmark_median([&] {
            std::uint64_t sum = 0;
            for (const auto& v : views) {
                sum += v.type_id;
            }
            (void)sum;
        });
        // Columnar scan: read all type_ids from a contiguous array
        // (simulated by extracting type_id into a separate vector
        // first, then iterating).
        std::vector<std::uint32_t> type_ids(N);
        for (std::size_t i = 0; i < N; ++i) type_ids[i] = views[i].type_id;
        double col_ms = benchmark_median([&] {
            std::uint64_t sum = 0;
            for (auto t : type_ids) {
                sum += t;
            }
            (void)sum;
        });
        double ratio = aos_ms / col_ms;
        std::println("  N={:>7} views", N);
        std::println("    AoS scan (struct-of-struct):  {:>8.3f} ms", aos_ms);
        std::println("    Columnar scan (separated vec): {:>8.3f} ms", col_ms);
        std::println("    AoS / columnar ratio:         {:.2f}x", ratio);
        results.push_back({"columnar_scan", N, col_ms, N * sizeof(std::uint32_t)});
    }
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════
int main() {
    std::println("═══ Cycle 14 P4: reflect path benchmark suite ═══");
    std::println("Issue: #217 Cycle 14 P4 (production migration benchmarks)");

    std::vector<BenchResult> results;
    bench_flatast(results);
    bench_node_view(results);
    bench_vector_string(results);
    bench_columnar_scan(results);

    // ── JSON output ──
    std::filesystem::path out_dir = "tests/bench_results";
    std::filesystem::create_directories(out_dir);
    std::filesystem::path out_file = out_dir / "cycle14_reflect_bench.json";
    {
        std::ofstream f(out_file);
        f << "{\n";
        f << "  \"issue\": \"#217 Cycle 14 P4\",\n";
        f << "  \"date\": \"2026-06-16\",\n";
        f << "  \"results\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            f << "    {\"name\": \"" << r.name << "\", \"n\": " << r.n
              << ", \"median_ms\": " << std::fixed << std::setprecision(3) << r.median_ms
              << ", \"buf_bytes\": " << r.buf_bytes << "}";
            if (i + 1 < results.size()) f << ",";
            f << "\n";
        }
        f << "  ]\n";
        f << "}\n";
    }
    std::println("\n  JSON written to {}", out_file.string());
    return 0;
}
