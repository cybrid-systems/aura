// @category: unit
// @reason: Issue #4936 — mutation-record wire bytes stay identical through
//          the memcpy helpers, including the version-2 provenance tail.
//
//   AC1: version 2 writer matches a field-by-field memcpy layout
//   AC2: version 2 reader returns every field and consumes the buffer
//   AC3: version 1 omits the 24-byte provenance tail
//   AC4: a short version-2 buffer leaves provenance at 0

import std;
import aura.core.mutation;

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

template <class T> void append_pod(std::vector<char>& buf, const T& value) {
    const auto at = buf.size();
    buf.resize(at + sizeof(T));
    std::memcpy(buf.data() + at, &value, sizeof(T));
}

void append_string(std::vector<char>& buf, std::string_view text) {
    append_pod(buf, static_cast<std::uint32_t>(text.size()));
    buf.insert(buf.end(), text.begin(), text.end());
}

aura::ast::MutationRecord sample() {
    aura::ast::MutationRecord rec;
    rec.mutation_id = 0x1122334455667788ull;
    rec.timestamp_ms = 42;
    rec.target_node = 7;
    rec.operator_name = "rebind";
    rec.old_type_str = "Int";
    rec.new_type_str = "String";
    rec.summary = "a b";
    rec.status = aura::ast::MutationStatus::Committed;
    rec.field_offset = 3;
    rec.old_value = 9;
    rec.new_value = 11;
    rec.has_rollback_data = true;
    rec.parent_id = 4;
    rec.child_idx = 1;
    rec.old_subtree_source = "old";
    rec.has_subtree_rollback = false;
    rec.invariant_status = aura::ast::InvariantStatus::NotChecked;
    rec.author_fingerprint = 99;
    rec.parent_mutation_id = 5;
    rec.composite_transaction_id = 6;
    return rec;
}

std::vector<char> manual_bytes(const aura::ast::MutationRecord& rec, bool provenance) {
    std::vector<char> buf;
    append_pod(buf, rec.mutation_id);
    append_pod(buf, rec.timestamp_ms);
    append_pod(buf, rec.target_node);
    append_string(buf, rec.operator_name);
    append_string(buf, rec.old_type_str);
    append_string(buf, rec.new_type_str);
    append_string(buf, rec.summary);
    buf.push_back(static_cast<char>(static_cast<std::uint8_t>(rec.status)));
    append_pod(buf, rec.field_offset);
    append_pod(buf, rec.old_value);
    append_pod(buf, rec.new_value);
    buf.push_back(rec.has_rollback_data ? '\1' : '\0');
    append_pod(buf, rec.parent_id);
    append_pod(buf, rec.child_idx);
    append_string(buf, rec.old_subtree_source);
    buf.push_back(rec.has_subtree_rollback ? '\1' : '\0');
    buf.push_back(static_cast<char>(static_cast<std::uint8_t>(rec.invariant_status)));
    if (provenance) {
        append_pod(buf, rec.author_fingerprint);
        append_pod(buf, rec.parent_mutation_id);
        append_pod(buf, rec.composite_transaction_id);
    }
    return buf;
}

bool same_body(const aura::ast::MutationRecord& got, const aura::ast::MutationRecord& rec) {
    return got.mutation_id == rec.mutation_id && got.timestamp_ms == rec.timestamp_ms &&
           got.target_node == rec.target_node && got.operator_name == rec.operator_name &&
           got.old_type_str == rec.old_type_str && got.new_type_str == rec.new_type_str &&
           got.summary == rec.summary && got.status == rec.status &&
           got.field_offset == rec.field_offset && got.old_value == rec.old_value &&
           got.new_value == rec.new_value && got.has_rollback_data == rec.has_rollback_data &&
           got.parent_id == rec.parent_id && got.child_idx == rec.child_idx &&
           got.old_subtree_source == rec.old_subtree_source &&
           got.has_subtree_rollback == rec.has_subtree_rollback &&
           got.invariant_status == rec.invariant_status;
}

} // namespace

int main() {
    using aura::ast::mutation::kMutationRecordProvenanceWireVersion;
    using aura::ast::mutation::wire_read_mutation_record;
    using aura::ast::mutation::wire_write_mutation_record;
    const auto rec = sample();

    std::vector<char> v2;
    wire_write_mutation_record(v2, rec, kMutationRecordProvenanceWireVersion);
    const auto expect_v2 = manual_bytes(rec, true);
    expect(v2 == expect_v2, "v2 bytes match the field layout");

    std::size_t pos = 0;
    const auto back = wire_read_mutation_record(v2, pos, kMutationRecordProvenanceWireVersion);
    expect(pos == v2.size() && same_body(back, rec) && back.author_fingerprint == 99 &&
               back.parent_mutation_id == 5 && back.composite_transaction_id == 6,
           "v2 roundtrip");

    std::vector<char> v1;
    wire_write_mutation_record(v1, rec, 1);
    expect(v1 == manual_bytes(rec, false), "v1 bytes omit the provenance tail");
    expect(v1.size() + 24 == v2.size(), "provenance tail is 24 bytes");
    pos = 0;
    const auto old = wire_read_mutation_record(v1, pos, 1);
    expect(pos == v1.size() && same_body(old, rec) && old.author_fingerprint == 0 &&
               old.parent_mutation_id == 0 && old.composite_transaction_id == 0,
           "v1 read leaves provenance at 0");

    pos = 0;
    const auto short_v2 = wire_read_mutation_record(v1, pos, kMutationRecordProvenanceWireVersion);
    expect(pos == v1.size() && same_body(short_v2, rec) && short_v2.author_fingerprint == 0 &&
               short_v2.parent_mutation_id == 0 && short_v2.composite_transaction_id == 0,
           "short v2 buffer leaves provenance at 0");

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
