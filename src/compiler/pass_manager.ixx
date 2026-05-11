export module aura.compiler.pass_manager;
import std;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.arity;

namespace aura::compiler {

// ── IRPass — base class for all compilation passes ──────────────
//
// A pass operates on an IRModule and may produce analysis results.
// Passes declare dependencies: the PassManager ensures they run
// in the correct order.
//
export class IRPass {
public:
    virtual ~IRPass() = default;

    // Unique pass name (for dependency lookup and diagnostics)
    virtual std::string_view name() const = 0;

    // Names of passes that must run before this one
    virtual std::vector<std::string_view> dependencies() const { return {}; }

    // Execute the pass on the given module
    virtual void run(aura::ir::IRModule& module) = 0;
};

// ── ComputeKindWrap — adapts compute-kind analysis as an IR pass ─
export class ComputeKindWrap final : public IRPass {
public:
    std::string_view name() const override { return "compute-kind"; }
    std::vector<std::string_view> dependencies() const override { return {}; }

    void run(aura::ir::IRModule& module) override {
        results_.clear();
        ComputeKindAnalysis analyzer;
        for (auto& func : module.functions) {
            results_.push_back(analyzer.analyze(func));
        }
    }

    const std::vector<ComputeKindResult>& results() const { return results_; }

private:
    std::vector<ComputeKindResult> results_;
};

// ── ArityWrap — adapts arity checking as an IR pass ─────────────
export class ArityWrap final : public IRPass {
public:
    std::string_view name() const override { return "arity"; }
    std::vector<std::string_view> dependencies() const override {
        return {"compute-kind"};  // wants compute-kind results
    }

    void run(aura::ir::IRModule& module) override {
        ArityChecker checker;
        result_ = checker.check(module);
    }

    const ArityCheckResult& result() const { return result_; }
    bool has_error() const { return result_.has_error; }

private:
    ArityCheckResult result_;
};

// ── ConstantFoldingWrap — stub for L2.5 (will leverage compute-kind) ─
export class ConstantFoldingWrap final : public IRPass {
public:
    std::string_view name() const override { return "const-fold"; }
    std::vector<std::string_view> dependencies() const override {
        return {"compute-kind"};
    }

    void run(aura::ir::IRModule& module) override {
        // Placeholder — L2.5 implementation will go here
        folded_ = false;
    }

    bool did_fold() const { return folded_; }

private:
    bool folded_ = false;
};

// ── PassManager — pass registry + dependency-aware executor ─────
//
// Usage:
//   PassManager pm;
//   auto& ck = pm.emplace<ComputeKindWrap>();
//   auto& ar = pm.emplace<ArityWrap>();
//   pm.run(module);
//
//   if (ar.has_error()) { /* handle arity errors */ }
//
export class PassManager {
public:
    ~PassManager() = default;

    // Construct and register a pass of type T with optional args
    template <std::derived_from<IRPass> T, typename... Args>
    T& emplace(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = ptr.get();
        passes_.push_back(std::move(ptr));
        return *raw;
    }

    // Run all registered passes in dependency order
    bool run(aura::ir::IRModule& module) {
        auto order = topo_sort();
        if (order.empty() && !passes_.empty()) {
            std::println(std::cerr, "PM: dependency cycle detected");
            return false;
        }

        for (auto idx : order) {
            auto& pass = passes_[idx];
            std::println(std::cerr, "PM: running {}", pass->name());
            pass->run(module);
            completed_.insert(std::string(pass->name()));
        }
        return true;
    }

    // Check if a named pass has been executed
    bool has_run(std::string_view name) const {
        return completed_.contains(std::string(name));
    }

    // Clear all passes and reset state
    void clear() {
        passes_.clear();
        completed_.clear();
    }

    // Number of registered passes
    std::size_t count() const { return passes_.size(); }

private:
    // Topological sort based on dependency declarations
    // Kahn's algorithm: O(V + E)
    std::vector<std::size_t> topo_sort() const {
        auto n = passes_.size();
        std::vector<std::size_t> in_degree(n, 0);
        std::unordered_map<std::size_t, std::vector<std::size_t>> edges;
        std::unordered_map<std::string_view, std::size_t> name_to_idx;

        for (std::size_t i = 0; i < n; ++i)
            name_to_idx[passes_[i]->name()] = i;

        for (std::size_t i = 0; i < n; ++i) {
            for (auto& dep_name : passes_[i]->dependencies()) {
                auto it = name_to_idx.find(dep_name);
                if (it != name_to_idx.end()) {
                    edges[it->second].push_back(i);
                    ++in_degree[i];
                } else {
                    std::println(std::cerr, "PM: pass '{}' requires '{}' but it's not registered",
                                 passes_[i]->name(), dep_name);
                }
            }
        }

        // Kahn's algorithm
        std::vector<std::size_t> result;
        std::queue<std::size_t> q;
        for (std::size_t i = 0; i < n; ++i)
            if (in_degree[i] == 0) q.push(i);

        while (!q.empty()) {
            auto idx = q.front(); q.pop();
            result.push_back(idx);
            for (auto& next : edges[idx]) {
                if (--in_degree[next] == 0)
                    q.push(next);
            }
        }

        return result;
    }

    std::vector<std::unique_ptr<IRPass>> passes_;
    std::unordered_set<std::string> completed_;
};

} // namespace aura::compiler
