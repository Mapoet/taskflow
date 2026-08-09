#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

#include "agent/observability/audit.hpp"
#include "agent/prompt_renderer/prompt_renderer.hpp"
#include "agent/telemetry/runtime.hpp"
#include "phase4_llm_runtime_test_support.hpp"

int main() {
    using namespace phase4_llm_test;
    auto store = std::make_shared<InMemoryLLMRuntimeStore>();
    publish_baseline(store);
    auto router = std::make_shared<ModelRouter>();
    assert(router->register_candidate(candidate("primary", "fake-primary", "model-a", 10)));
    assert(router->register_candidate(candidate("fallback", "fake-fallback", "model-b", 20)));

    auto primary = std::make_shared<ScriptedAdapter>();
    primary->push([]() -> LLMOutput {
        throw llm_http_error(503, "fake-primary", "temporary outage");
    });
    auto fallback = std::make_shared<ScriptedAdapter>();
    fallback->push([] { return output("not-json", 11, 3); });
    fallback->push([] { return output(R"({"ok":true})", 12, 4); });

    auto client = std::make_shared<LLMClient>();
    client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    client->register_adapter("fake-primary", primary);
    client->register_adapter("fake-fallback", fallback);

    auto telemetry_sink = std::make_shared<telemetry::InMemoryTelemetrySink>();
    auto telemetry_runtime = std::make_shared<telemetry::TelemetryRuntime>(telemetry_sink,
        telemetry::TelemetryPolicy{{"llm.invocation.id", "llm.role", "llm.profile.id",
            "llm.profile.revision", "llm.prompt.revision", "llm.provider", "llm.model",
            "llm.route.digest", "llm.fallback.count", "memory.view.digest",
            "llm.usage.input_tokens", "llm.usage.output_tokens", "llm.cost.usd"}, 256});
    auto audit = std::make_shared<TestAuditSink>();
    RoleRuntime runtime(client, store, router, telemetry_runtime, audit);

    std::string streamed;
    std::string summary;
    const auto result = runtime.invoke(request(),
        [&](std::string_view value) { streamed += value; },
        [&](std::string_view value) { summary += value; });
    assert(result.ok && result.structured_output && result.structured_output->at("ok") == true);
    assert(primary->calls() == 1 && fallback->calls() == 2);
    assert(result.manifest.attempts.size() == 3);
    assert(result.manifest.attempts[1].fallback);
    assert(result.manifest.attempts[2].output_repair);
    assert(result.manifest.usage.input_tokens == 23);
    assert(result.manifest.usage.output_tokens == 7);
    assert(streamed == R"({"ok":true})");
    assert(summary == "provider-visible-summaryprovider-visible-summary");
    assert(fallback->rendered().back().model_config);
    assert(fallback->rendered().back().model_config->model_name == "model-b");
    assert(fallback->rendered().back().model_config->max_retries == 0);
    assert(fallback->rendered().back().model_config->extra_params.at("seed") == 7);

    const std::string durable = encode(result.manifest).dump();
    assert(durable.find("private-chain-of-thought-do-not-persist") == std::string::npos);
    assert(telemetry_sink->spans().size() == 1);
    assert(audit->events_for_trace("trace-a").size() == 2);
    const auto stored = store->load_invocation("tenant-a", "invocation-a");
    assert(stored && stored->manifest.state == InvocationState::Succeeded);

    const auto duplicate = runtime.invoke(request());
    assert(!duplicate.ok && duplicate.error_code == "invocation_already_exists");
    assert(primary->calls() == 1 && fallback->calls() == 2);

    auto malformed = request("invocation-b");
    malformed.prompt_variables.clear();
    const auto rejected = runtime.invoke(malformed);
    assert(!rejected.ok && rejected.error_code == "prompt_render_failed");
    assert(primary->calls() == 1);

    auto independent = request("invocation-c");
    independent.independence.require_provider_diversity = true;
    const auto denied = runtime.invoke(independent);
    assert(!denied.ok && denied.error_code == "provider_diversity_evidence_missing");
    assert(primary->calls() == 1);
    assert(telemetry_sink->spans().size() == 2);
    assert(audit->events_for_trace("trace-a").size() == 3);

    // Calibration is checked again after routing a fallback; an approved primary
    // model must not implicitly authorize a different provider/model pair.
    auto pinned_store = std::make_shared<InMemoryLLMRuntimeStore>();
    assert(pinned_store->publish_profile(profile()).ok());
    assert(pinned_store->publish_prompt(prompt()).ok());
    assert(pinned_store->publish_calibration(
        calibration("fake-primary", "model-a")).ok());
    auto pinned_router = std::make_shared<ModelRouter>();
    assert(pinned_router->register_candidate(candidate("primary", "fake-primary", "model-a", 10)));
    assert(pinned_router->register_candidate(candidate("fallback", "fake-fallback", "model-b", 20)));
    auto pinned_primary = std::make_shared<ScriptedAdapter>();
    pinned_primary->push([]() -> LLMOutput {
        throw llm_http_error(503, "fake-primary", "temporary outage");
    });
    auto pinned_fallback = std::make_shared<ScriptedAdapter>();
    pinned_fallback->push([] { return output(R"({"ok":true})"); });
    auto pinned_client = std::make_shared<LLMClient>();
    pinned_client->set_prompt_renderer(std::make_shared<PromptRenderer>());
    pinned_client->register_adapter("fake-primary", pinned_primary);
    pinned_client->register_adapter("fake-fallback", pinned_fallback);
    RoleRuntime pinned_runtime(pinned_client, pinned_store, pinned_router);
    const auto calibration_denied = pinned_runtime.invoke(request("invocation-d"));
    assert(!calibration_denied.ok && calibration_denied.error_code == "calibration_denied");
    assert(pinned_primary->calls() == 1 && pinned_fallback->calls() == 0);
    return 0;
}
