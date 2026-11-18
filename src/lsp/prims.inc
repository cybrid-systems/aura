// Primitive signatures and descriptions (#5261).
//
// The REPL's `:doc NAME` prints `Primitives::meta_for_slot(slot).doc` and
// `:type NAME` prints the same slot's arity. Hover, signatureHelp and
// completion read that one registry through a CompilerService's evaluator —
// the same object the REPL drives — so there is exactly one description per
// primitive and no second table to drift. The process test compares hover
// against `:doc` on the live binary.
//
// Nothing here adds an Aura primitive (#5002): this is an internal C++ view.
#ifndef AURA_LSP_PRIMS_INC
#define AURA_LSP_PRIMS_INC

namespace lsp_4911 {

struct PrimView {
    std::string name;
    std::string signature; // "name : schema", or "name/arity" as :type shows it
    std::string doc;
    std::string params; // the schema's parameter group, e.g. "string int"
    int arity = 255;    // 255 = variadic, as PrimMeta

    // The call shape for signatureHelp: the declared parameter list when the
    // registry has a schema, otherwise one placeholder per positional slot.
    std::string call_label() const {
        if (!params.empty())
            return "(" + name + " " + params + ")";
        if (arity == 255)
            return "(" + name + " ...)";
        if (arity == 0)
            return "(" + name + ")";
        std::string out = "(" + name;
        for (int i = 0; i < arity; ++i)
            out += " arg" + std::to_string(i + 1);
        return out + ")";
    }
};

// The parameter group of a schema like "(string int) -> bool"; empty when the
// schema has no leading group.
inline std::string schema_params(std::string_view schema) {
    if (schema.empty() || schema.front() != '(')
        return {};
    int depth = 0;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (schema[i] == '(')
            ++depth;
        else if (schema[i] == ')' && --depth == 0)
            return std::string(schema.substr(1, i - 1));
    }
    return {};
}

struct PrimRegistryView {
    std::unique_ptr<aura::compiler::CompilerService> rt;
    std::vector<PrimView> items;
    std::unordered_map<std::string, std::size_t> by_name;
    bool loaded = false;

    // Built on first use: a CompilerService installs the full primitive
    // registry in its constructor, exactly as the REPL's session does.
    void ensure() {
        if (loaded)
            return;
        loaded = true;
        try {
            rt = std::make_unique<aura::compiler::CompilerService>();
            const auto& prims = rt->evaluator().primitives();
            items.reserve(prims.slot_count());
            for (std::size_t slot = 0; slot < prims.slot_count(); ++slot) {
                PrimView v;
                v.name = prims.name_for_slot(slot);
                if (v.name.empty())
                    continue;
                const auto& meta = prims.meta_for_slot(slot);
                v.doc = meta.doc;
                v.arity = meta.arity;
                v.params = schema_params(meta.schema);
                if (!meta.schema.empty())
                    v.signature = v.name + " : " + meta.schema;
                else
                    v.signature =
                        v.name + "/" + (meta.arity == 255 ? "*" : std::to_string(meta.arity));
                if (!by_name.contains(v.name))
                    by_name.emplace(v.name, items.size());
                items.push_back(std::move(v));
            }
        } catch (const std::exception&) {
            // A registry that cannot be built leaves every lookup empty; the
            // handlers degrade to define-only answers instead of failing.
            items.clear();
            by_name.clear();
        }
    }

    const PrimView* find(std::string_view name) {
        if (name.empty())
            return nullptr;
        ensure();
        const auto it = by_name.find(std::string(name));
        return it == by_name.end() ? nullptr : &items[it->second];
    }
};

// Parameter labels for signatureHelp, in call order: the schema's own words
// when the registry has one, otherwise the call shape's placeholders.
inline std::vector<std::string> prim_param_labels(const PrimView& prim) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < prim.params.size()) {
        while (i < prim.params.size() && std::isspace(static_cast<unsigned char>(prim.params[i])))
            ++i;
        const std::size_t b = i;
        while (i < prim.params.size() && !std::isspace(static_cast<unsigned char>(prim.params[i])))
            ++i;
        if (i > b)
            out.emplace_back(prim.params.substr(b, i - b));
    }
    if (!out.empty())
        return out;
    if (prim.arity == 255)
        return {"..."};
    for (int k = 0; k < prim.arity; ++k)
        out.push_back("arg" + std::to_string(k + 1));
    return out;
}

} // namespace lsp_4911

#endif // AURA_LSP_PRIMS_INC
