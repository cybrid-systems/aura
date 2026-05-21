// ──────────────────────────────────────────────────────────────
//  read_auto_validate.hh — P2996 struct layout validation
//
//  Standalone header (no module dependency). Uses P2996 to
//  verify that AST node structs have the expected ABF layout
//  at compile time: first member is NodeTag, remaining members
//  match the expected ABF read pattern.
//
//  Run: g++ -std=c++26 -freflection -I. this_file.cpp
// ──────────────────────────────────────────────────────────────

#ifndef AURA_REFLECT_READ_AUTO_VALIDATE_HH
#define AURA_REFLECT_READ_AUTO_VALIDATE_HH

#include <meta>
#include <cstdio>
#include <string>

namespace aura::reflect::abf_validate {

template <typename T>
consteval const char* validate_node() {
    using namespace std::meta;
    auto members = nonstatic_data_members_of(^^T, access_context::unchecked());
    
    // Must have at least one member (tag)
    if (members.size() < 1)
        return "must have at least one member (tag)";
    
    // First member must be NodeTag (enum)
    auto first = members.data()[0];
    auto first_type = type_of(first);
    if (!is_enum_type(first_type))
        return "first member must be NodeTag enum";
    if (size_of(first_type) != 4)
        return "NodeTag must be uint32_t sized";
    
    // Remaining members must be ABF-serializable types
    for (std::size_t i = 1; i < members.size(); ++i) {
        auto m = members.data()[i];
        auto t = type_of(m);
        auto type_disp = display_string_of(t);
        
        // Check type via is_same_type where possible, fallback to display_string
        auto d = display_string_of(t);
        bool ok = d == "long int" || d == "unsigned long"
               || d == "int" || d == "unsigned int"
               || d == "short" || d == "unsigned short"
               || d.starts_with("std::string")
               || d.find("std::__cxx11::basic_string") != std::string_view::npos
               || d.starts_with("Expr*")
               || d.find("vector<") != std::string_view::npos;
        
        if (!ok)
            return "member type not ABF-serializable";
    }
    
    return nullptr;  // valid
}

// Run validation at compile time (call from consteval context)
template <typename T>
consteval void check() {
    constexpr auto err = validate_node<T>();
    if (err) {
        static_assert(false, "validation failed");
    }
}

// Get member count (consteval)
template <typename T>
consteval std::size_t member_count() {
    return std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::unchecked()).size();
}

} // namespace aura::reflect::abf_validate

#endif
