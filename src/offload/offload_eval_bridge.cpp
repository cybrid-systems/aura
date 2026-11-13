// offload/offload_eval_bridge.cpp — Issue #4887
//
// Adapter from the evaluator-facing OffloadEval interface to OffloadService.
// This TU is linked into the aura binary only; evaluator/test targets see the
// header-only interface and the g_offload_eval pointer (null there).
#include "offload/offload_eval.h"

#include "offload/offload_job.h"
#include "offload/offload_service.h"


namespace aura::offload {
namespace {

    class EvalAdapter final : public OffloadEval {
    public:
        void set_source(std::string source) {
            std::scoped_lock lock(mu_);
            source_ = std::move(source);
        }

        [[nodiscard]] std::string eval_async(const std::string& code) override {
            std::string source;
            {
                std::scoped_lock lock(mu_);
                source = source_;
            }
            auto input = std::make_shared<const SnapshotInput>(SnapshotInput{std::move(source), 0});
            auto job = [code](const SnapshotInput& in, std::stop_token st) {
                Result out;
                if (st.stop_requested())
                    return out;
                out.artifact = eval_code(in, code);
                return out;
            };
            auto& service = OffloadService::instance();
            auto sub = service.submit(std::move(job), input, OffloadClass::Interactive);
            if (sub.status != SubmitStatus::Accepted)
                return {};
            service.await(sub.ticket);
            if (auto* result = std::get_if<Result>(&sub.ticket->out))
                return result->artifact;
            if (auto* error = std::get_if<Error>(&sub.ticket->out))
                return error->message;
            return {};
        }

    private:
        std::mutex mu_;
        std::string source_;
    };

    EvalAdapter& adapter_instance() {
        static EvalAdapter adapter;
        return adapter;
    }

} // namespace

OffloadEval& offload_eval_instance() {
    return adapter_instance();
}

void set_offload_eval_source(std::string source) {
    adapter_instance().set_source(std::move(source));
}

} // namespace aura::offload
