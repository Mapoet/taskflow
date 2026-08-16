#include "agent/llm_runtime/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "agent/observability/audit.hpp"
#include "agent/telemetry/runtime.hpp"

namespace agent_framework::llm_runtime {
namespace {

using json = nlohmann::json;

std::atomic<std::uint64_t> invocation_counter{0};

std::string default_now() { return audit_timestamp_now(); }

std::string default_id() {
    const auto sequence = invocation_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const json basis = {{"time", default_now()}, {"sequence", sequence}};
    const auto digest = contracts::embedded_digest(basis).value_or("sha256:unknown");
    return "llm-" + digest.substr(digest.find(':') + 1, 24);
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::string digest_strings(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return contracts::embedded_digest(values).value_or("");
}

json prompt_input_json(const std::map<std::string, std::string>& values) {
    json result = json::object();
    for(const auto& [key, value] : values) result[key] = value;
    return result;
}

std::string input_digest(const RoleInvocationRequest& request, const LLMInput& input,
                         const RenderedRolePrompt& prompt) {
    json history = json::array();
    for(const auto& message : input.history)
        history.push_back({{"role",message.role},{"content",message.content},
                           {"tool_name",message.tool_name.value_or("")}});
    json tools = json::array();
    for(const auto& tool : input.tools)
        tools.push_back({{"name",tool.name},{"schema",tool.schema}});
    return contracts::embedded_digest({{"system_prompt",prompt.system_prompt},
        {"user_prompt",prompt.user_prompt},{"context",input.context},{"history",history},
        {"tools",tools},{"memory_view_digest",request.memory_view.view_digest},
        {"context_projection_digest", request.metadata.extensions.value(
             "context_projection_digest", std::string{})},
        {"prompt_digest",prompt.prompt_digest}}).value_or("");
}

std::string output_digest(const LLMOutput& output) {
    json tools = json::array();
    for(const auto& call : output.tool_calls)
        tools.push_back({{"name",call.name},{"arguments",call.arguments},
                         {"tool_call_id",call.tool_call_id.value_or("")}});
    // Provider-private reasoning is intentionally excluded from durable output identity.
    return contracts::embedded_digest({{"final_answer",output.final_answer},
        {"tool_calls",tools},{"is_final",output.is_final},
        {"audio",output.audio_out.value_or("")},{"image",output.image_out.value_or("")}}).value_or("");
}

UsageRecord usage_record(const std::optional<LLMUsage>& usage) {
    UsageRecord result;
    if(!usage) {
        result.source = "unknown";
        result.unknown_reason = "provider did not report usage";
        return result;
    }
    result.input_tokens=usage->input_tokens; result.output_tokens=usage->output_tokens;
    result.cached_input_tokens=usage->cached_input_tokens; result.cost_usd=usage->cost_usd;
    result.source=usage->source; result.unknown_reason=usage->unknown_reason;
    return result;
}

void merge_usage(UsageRecord& total, const UsageRecord& next) {
    const auto add_u64=[](std::optional<std::uint64_t>& target,
                          const std::optional<std::uint64_t>& value) {
        if(value) target = target.value_or(0) + *value;
    };
    add_u64(total.input_tokens,next.input_tokens); add_u64(total.output_tokens,next.output_tokens);
    add_u64(total.cached_input_tokens,next.cached_input_tokens);
    if(next.cost_usd) total.cost_usd=total.cost_usd.value_or(0.0)+*next.cost_usd;
    if(!next.source.empty()) total.source = total.source.empty() ? next.source : total.source + "," + next.source;
    if(!next.unknown_reason.empty()) total.unknown_reason = next.unknown_reason;
}

FailureClass classify(const llm_http_error& error) {
    return error.status_code == 0 || error.status_code == 408 || error.status_code == 429 ||
           error.status_code >= 500 ? FailureClass::Retryable : FailureClass::NonRetryable;
}

std::string error_code(const llm_http_error& error) {
    return error.status_code == 0 ? "provider_transport_error"
                                  : "provider_http_" + std::to_string(error.status_code);
}

void apply_route(LLMInvocationManifest& manifest, const ModelRouteDecision& route) {
    manifest.route_decision_digest = encode(route).at("canonical_digest").get<std::string>();
    manifest.candidate_id=route.candidate_id; manifest.provider=route.provider;
    manifest.model=route.model; manifest.adapter_revision=route.adapter_revision;
}

bool tool_capabilities_allowed(const RoleInvocationRequest& request, std::string* denied_tool) {
    for(const auto& tool : request.input.tools) {
        if(!contains(request.granted_capabilities, tool.name) &&
           !contains(request.granted_capabilities, "tool:" + tool.name)) {
            if(denied_tool) *denied_tool=tool.name;
            return false;
        }
    }
    return true;
}

ModelConfig request_model_config(const LLMRoleProfile& profile,
                                 const ModelRouteDecision& route,
                                 const LLMInput& input) {
    ModelConfig config = input.model_config.value_or(ModelConfig{});
    config.model_name=route.model; config.temperature=profile.temperature; config.top_p=profile.top_p;
    config.max_tokens=profile.max_output_tokens; config.http_timeout_sec=std::max(1,profile.timeout_ms/1000);
    config.max_retries=0;
    config.extra_params=profile.provider_parameters;
    if(route.provider=="openai") config.extra_params["reasoning_effort"]=reasoning_effort_name(profile.reasoning_effort);
    return config;
}

bool calibration_matches(const RoleCalibrationRecord& calibration,
                         const LLMRoleProfile& profile,
                         const ModelRouteDecision& route) {
    return calibration.approved && calibration.profile_id==profile.profile_id &&
           calibration.profile_revision==profile.revision &&
           calibration.prompt_revision==profile.prompt_revision &&
           (calibration.provider.empty() || calibration.provider==route.provider) &&
           (calibration.model.empty() || calibration.model==route.model);
}

}  // namespace

RoleRuntime::RoleRuntime(std::shared_ptr<LLMClient> client,
    std::shared_ptr<LLMRuntimeStore> store, std::shared_ptr<ModelRouter> router,
    std::shared_ptr<telemetry::TelemetryRuntime> telemetry,
    std::shared_ptr<AuditSink> audit, RoleRuntimeOptions options)
    : client_(std::move(client)), store_(std::move(store)), router_(std::move(router)),
      telemetry_(std::move(telemetry)), audit_(std::move(audit)), options_(std::move(options)),
      profiles_(store_), prompts_(store_) {
    if(!client_ || !store_ || !router_) throw std::invalid_argument("role runtime dependencies are required");
    if(!options_.now) options_.now=default_now;
    if(!options_.next_id) options_.next_id=default_id;
}

RoleInvocationResult RoleRuntime::invoke(
    RoleInvocationRequest request,
    std::function<void(std::string_view)> answer_callback,
    std::function<void(std::string_view)> thinking_summary_callback) {
    RoleInvocationResult result;
    if(request.invocation_id.empty()) request.invocation_id=options_.next_id();
    if(request.metadata.identity.tenant_id.empty() || request.metadata.identity.task_id.empty() ||
       request.profile_id.empty() || request.profile_revision.empty()) {
        result.error_code="invocation_identity_missing";
        result.error_message="tenant, task, profile id and pinned revision are required";
        return result;
    }
    const auto profile=profiles_.resolve(request.metadata.identity.tenant_id,
                                         request.profile_id,request.profile_revision);
    if(!profile) {
        result.error_code="profile_not_found"; result.error_message="pinned role profile was not found";
        return result;
    }
    const auto prompt=prompts_.resolve(request.metadata.identity.tenant_id,
                                       profile->prompt_id,profile->prompt_revision);
    if(!prompt || prompt->deprecated) {
        result.error_code=prompt ? "prompt_deprecated" : "prompt_not_found";
        result.error_message="pinned prompt revision is unavailable";
        return result;
    }
    std::string prompt_error;
    const auto rendered=prompts_.render(*prompt,request.prompt_variables,&prompt_error);
    if(!rendered) {
        result.error_code="prompt_render_failed"; result.error_message=prompt_error;
        return result;
    }

    LLMInput input=request.input;
    input.system_prompt=rendered->system_prompt;
    input.user_prompt=rendered->user_prompt;
    const json structured_input=prompt_input_json(request.prompt_variables);
    json input_validation_error;
    std::string policy_error;
    if(!output_gate_.validate_input(*prompt,structured_input,&input_validation_error))
        policy_error="prompt_input_schema_failed";
    else if(!tool_capabilities_allowed(request,&prompt_error))
        policy_error="tool_capability_denied:"+prompt_error;
    else if(!profile->memory_view_profile.empty() &&
            (request.memory_view.snapshot_id.empty() || request.memory_view.view_digest.empty()))
        policy_error="memory_view_binding_missing";
    else if(request.metadata.extensions.value(
                "context_projection_required", false) &&
            request.metadata.extensions.value(
                "context_projection_digest", std::string{}).empty())
        policy_error="context_projection_binding_missing";

    ModelRouteDecision route=router_->route(*profile,request);
    if(!policy_error.empty()) {
        route.selected=false; route.candidate_id.clear(); route.provider.clear(); route.model.clear();
        route.adapter_revision.clear(); route.decision_code=policy_error;
        route.decision_message="invocation failed deterministic input/capability/view policy";
    }
    if(route.selected && options_.require_approved_calibration) {
        const auto calibration=store_->load_calibration(request.metadata.identity.tenant_id,
                                                         profile->calibration_revision);
        if(profile->calibration_revision.empty() || !calibration ||
           !calibration_matches(*calibration,*profile,route)) {
            route.selected=false; route.decision_code="calibration_denied";
            route.decision_message="selected role/profile/prompt/model has no approved calibration";
        }
    }

    LLMInvocationManifest manifest;
    manifest.metadata=request.metadata; manifest.invocation_id=request.invocation_id;
    manifest.state=InvocationState::Pending; manifest.role=profile->role;
    manifest.profile_id=profile->profile_id; manifest.profile_revision=profile->revision;
    manifest.prompt_id=prompt->prompt_id; manifest.prompt_revision=prompt->revision;
    manifest.prompt_digest=rendered->prompt_digest; apply_route(manifest,route);
    manifest.reasoning_effort=reasoning_effort_name(profile->reasoning_effort);
    manifest.independence_group=profile->independence_group;
    manifest.evidence_authority=evidence_authority_name(profile->evidence_authority);
    manifest.calibration_revision=profile->calibration_revision;
    manifest.memory_snapshot_id=request.memory_view.snapshot_id;
    manifest.memory_view_profile=request.memory_view.profile;
    manifest.memory_view_digest=request.memory_view.view_digest;
    manifest.capabilities=request.granted_capabilities;
    manifest.capability_digest=digest_strings(request.granted_capabilities);
    manifest.input_digest=input_digest(request,input,*rendered);
    manifest.started_at=options_.now();

    auto store_result=store_->create_invocation(manifest);
    if(store_result.status==RuntimeStoreStatus::AlreadyExists) {
        const auto existing=store_->load_invocation(request.metadata.identity.tenant_id,request.invocation_id);
        if(existing) result.manifest=existing->manifest;
        result.error_code="invocation_already_exists";
        result.error_message="idempotency key already has a durable invocation";
        return result;
    }
    if(!store_result.ok()) {
        result.error_code="invocation_store_create_failed"; result.error_message=store_result.message;
        return result;
    }
    std::uint64_t revision=store_result.revision;
    const auto started=std::chrono::steady_clock::now();
    const auto elapsed_ms=[&]() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-started).count());
    };

    const auto persist_terminal=[&](InvocationState state,const std::string& code,
                                    const std::string& message) {
        if(manifest.usage.source.empty()) {
            manifest.usage.source="unknown";
            manifest.usage.unknown_reason="provider did not report usage";
        }
        manifest.latency_ms=elapsed_ms();
        manifest.state=state; manifest.error_code=code; manifest.error_message=message;
        manifest.finished_at=options_.now();
        const auto committed=store_->update_invocation(manifest,revision);
        if(committed.ok()) revision=committed.revision;
        result.manifest=manifest; result.error_code=code; result.error_message=message;
    };

    if(!route.selected) {
        persist_terminal(InvocationState::Failed,route.decision_code,route.decision_message);
        if(telemetry_) {
            telemetry::SpanRecord span; span.context.metadata=request.metadata;
            span.context.trace_id=request.trace_id.empty()?manifest.invocation_id:request.trace_id;
            span.context.span_id=manifest.invocation_id; span.context.parent_span_id=request.parent_span_id;
            span.context.node_id=profile->role; span.context.memory_snapshot_id=manifest.memory_snapshot_id;
            span.context.memory_view_digest=manifest.memory_view_digest; span.name="llm.role.invoke";
            span.started_at=manifest.started_at; span.finished_at=manifest.finished_at; span.status="error";
            span.attributes={{"llm.invocation.id",manifest.invocation_id},{"llm.role",manifest.role},
                {"llm.profile.id",manifest.profile_id},{"llm.profile.revision",manifest.profile_revision},
                {"llm.prompt.revision",manifest.prompt_revision},{"llm.provider",manifest.provider},
                {"llm.model",manifest.model},{"llm.route.digest",manifest.route_decision_digest},
                {"llm.fallback.count","0"},{"memory.view.digest",manifest.memory_view_digest}};
            std::string ignored; telemetry_->emit_span(std::move(span),&ignored);
        }
        if(audit_) {
            AuditEvent event; event.timestamp=options_.now(); event.trace_id=request.trace_id;
            event.tenant_id=request.metadata.identity.tenant_id;
            event.task_id=request.metadata.identity.task_id; event.component="llm_role_runtime";
            event.event_kind="invocation_rejected"; event.capability_id=profile->role;
            event.capability_revision=profile->revision; event.outcome="failed";
            event.error_code=manifest.error_code; event.latency_ms=manifest.latency_ms;
            event.payload={{"invocation_id",manifest.invocation_id},
                {"manifest_digest",encode(manifest).at("canonical_digest")},
                {"route_decision",route.decision_code}};
            event.payload=redact_audit_payload(std::move(event.payload));
            event.payload_digest=audit_payload_digest(event.payload); audit_->write(event);
        }
        return result;
    }

    manifest.state=InvocationState::Running;
    store_result=store_->update_invocation(manifest,revision);
    if(!store_result.ok()) {
        result.error_code="invocation_store_start_failed"; result.error_message=store_result.message;
        return result;
    }
    revision=store_result.revision;

    if(audit_) {
        AuditEvent event; event.timestamp=options_.now(); event.trace_id=request.trace_id;
        event.tenant_id=request.metadata.identity.tenant_id; event.task_id=request.metadata.identity.task_id;
        event.component="llm_role_runtime"; event.event_kind="invocation_started";
        event.capability_id=profile->role; event.capability_revision=profile->revision;
        event.outcome="running"; event.payload={{"invocation_id",manifest.invocation_id},
            {"profile_id",manifest.profile_id},{"prompt_revision",manifest.prompt_revision},
            {"provider",manifest.provider},{"model",manifest.model},
            {"memory_view_digest",manifest.memory_view_digest}};
        event.payload=redact_audit_payload(std::move(event.payload));
        event.payload_digest=audit_payload_digest(event.payload); audit_->write(event);
    }

    std::vector<std::string> excluded;
    int fallbacks=0;
    int repairs=0;
    bool repair_mode=false;
    ModelRouteDecision current_route=route;
    const auto calibration = options_.require_approved_calibration
        ? store_->load_calibration(request.metadata.identity.tenant_id,
                                   profile->calibration_revision)
        : std::optional<RoleCalibrationRecord>{};
    for(int attempt_number=1;attempt_number<=profile->max_attempts;++attempt_number) {
        if(attempt_number>1 && !repair_mode) {
            current_route=router_->route(*profile,request,excluded);
            if(!current_route.selected) {
                persist_terminal(InvocationState::Failed,current_route.decision_code,current_route.decision_message);
                break;
            }
            apply_route(manifest,current_route);
        }
        if(options_.require_approved_calibration &&
           (profile->calibration_revision.empty() || !calibration ||
            !calibration_matches(*calibration,*profile,current_route))) {
            persist_terminal(InvocationState::Failed,"calibration_denied",
                "selected fallback role/profile/prompt/model has no approved calibration");
            break;
        }
        InvocationAttempt attempt;
        attempt.sequence=static_cast<std::uint64_t>(attempt_number);
        attempt.candidate_id=current_route.candidate_id; attempt.provider=current_route.provider;
        attempt.model=current_route.model; attempt.started_at=options_.now();
        attempt.fallback=fallbacks>0; attempt.output_repair=repair_mode;

        if(!client_->has_provider(current_route.provider)) {
            attempt.failure_class=FailureClass::Retryable; attempt.error_code="provider_not_registered";
            attempt.error_message="routed provider has no registered LLMClient adapter";
            attempt.finished_at=options_.now(); attempt.usage=usage_record(std::nullopt);
            manifest.attempts.push_back(attempt); merge_usage(manifest.usage,attempt.usage);
        } else {
            input.model_config=request_model_config(*profile,current_route,input);
            try {
                // Structured responses are not observable until the schema gate accepts
                // them. This prevents invalid/repaired drafts from escaping over a stream.
                const auto provider_answer_callback = prompt->structured_output_required
                    ? std::function<void(std::string_view)>{}
                    : answer_callback;
                const auto output=client_->invoke_channels(input,current_route.provider,
                    provider_answer_callback,thinking_summary_callback).get();
                attempt.finished_at=options_.now(); attempt.usage=usage_record(output.usage);
                const auto gated=output_gate_.validate_output(*prompt,output.final_answer);
                if(gated.accepted()) {
                    attempt.failure_class=FailureClass::None;
                    manifest.attempts.push_back(attempt); merge_usage(manifest.usage,attempt.usage);
                    manifest.output_digest=output_digest(output); manifest.state=InvocationState::Succeeded;
                    manifest.latency_ms=elapsed_ms();
                    manifest.finished_at=options_.now(); manifest.error_code.clear(); manifest.error_message.clear();
                    const auto committed=store_->update_invocation(manifest,revision);
                    if(!committed.ok()) {
                        result.error_code="invocation_store_finish_failed"; result.error_message=committed.message;
                        result.manifest=manifest; return result;
                    }
                    revision=committed.revision; result.ok=true; result.output=output;
                    result.structured_output=gated.value; result.manifest=manifest; break;
                }
                attempt.failure_class=FailureClass::OutputInvalid; attempt.error_code=gated.code;
                attempt.error_message=gated.message; manifest.attempts.push_back(attempt);
                merge_usage(manifest.usage,attempt.usage);
                if(repairs<prompt->max_repair_attempts && attempt_number<profile->max_attempts) {
                    ++repairs; repair_mode=true;
                    input.system_prompt += "\n\n"+output_gate_.repair_instruction(*prompt,gated);
                    const auto committed=store_->update_invocation(manifest,revision);
                    if(!committed.ok()) {
                        persist_terminal(InvocationState::ManualReview,"invocation_store_attempt_failed",committed.message);
                        break;
                    }
                    revision=committed.revision;
                    continue;
                }
                persist_terminal(InvocationState::Failed,"structured_output_exhausted",
                                 "structured output repair budget was exhausted");
                break;
            } catch(const llm_http_error& error) {
                attempt.finished_at=options_.now(); attempt.failure_class=classify(error);
                attempt.error_code=error_code(error); attempt.error_message=error.what();
                attempt.usage=usage_record(std::nullopt); manifest.attempts.push_back(attempt);
                merge_usage(manifest.usage,attempt.usage);
            } catch(const std::exception& error) {
                attempt.finished_at=options_.now();
                const bool cancelled=input.cancellation_requested && input.cancellation_requested();
                attempt.failure_class=cancelled?FailureClass::Cancelled:FailureClass::NonRetryable;
                attempt.error_code=cancelled?"invocation_cancelled":"provider_invocation_failed";
                attempt.error_message=error.what(); attempt.usage=usage_record(std::nullopt);
                manifest.attempts.push_back(attempt); merge_usage(manifest.usage,attempt.usage);
            }
        }

        const auto& failed=manifest.attempts.back();
        repair_mode=false;
        if(failed.failure_class==FailureClass::Cancelled) {
            persist_terminal(InvocationState::Cancelled,failed.error_code,failed.error_message); break;
        }
        if(failed.failure_class!=FailureClass::Retryable) {
            persist_terminal(InvocationState::Failed,failed.error_code,failed.error_message); break;
        }
        if(attempt_number>=profile->max_attempts) {
            persist_terminal(InvocationState::Failed,"attempt_budget_exhausted",
                             "provider attempt budget was exhausted"); break;
        }
        if(fallbacks<profile->max_fallbacks) {
            excluded.push_back(current_route.candidate_id); ++fallbacks;
        }
        const auto committed=store_->update_invocation(manifest,revision);
        if(!committed.ok()) {
            persist_terminal(InvocationState::ManualReview,"invocation_store_attempt_failed",committed.message);
            break;
        }
        revision=committed.revision;
    }
    if(!result.ok && result.error_code.empty())
        persist_terminal(InvocationState::Failed,"attempt_budget_exhausted","invocation did not produce an accepted result");

    if(result.ok && prompt->structured_output_required && answer_callback)
        answer_callback(result.output.final_answer);

    const auto elapsed=elapsed_ms();
    if(telemetry_) {
        telemetry::SpanRecord span; span.context.metadata=request.metadata;
        span.context.trace_id=request.trace_id.empty()?manifest.invocation_id:request.trace_id;
        span.context.span_id=manifest.invocation_id; span.context.parent_span_id=request.parent_span_id;
        span.context.node_id=profile->role; span.context.memory_snapshot_id=manifest.memory_snapshot_id;
        span.context.memory_view_digest=manifest.memory_view_digest; span.name="llm.role.invoke";
        span.started_at=manifest.started_at; span.finished_at=manifest.finished_at;
        span.status=result.ok?"ok":"error";
        span.attributes={{"llm.invocation.id",manifest.invocation_id},{"llm.role",manifest.role},
            {"llm.profile.id",manifest.profile_id},{"llm.profile.revision",manifest.profile_revision},
            {"llm.prompt.revision",manifest.prompt_revision},{"llm.provider",manifest.provider},
            {"llm.model",manifest.model},{"llm.route.digest",manifest.route_decision_digest},
            {"llm.fallback.count",std::to_string(fallbacks)},
            {"memory.view.digest",manifest.memory_view_digest}};
        if(manifest.usage.input_tokens) span.attributes["llm.usage.input_tokens"]=std::to_string(*manifest.usage.input_tokens);
        if(manifest.usage.output_tokens) span.attributes["llm.usage.output_tokens"]=std::to_string(*manifest.usage.output_tokens);
        if(manifest.usage.cost_usd) {
            std::ostringstream value; value<<std::setprecision(12)<<*manifest.usage.cost_usd;
            span.attributes["llm.cost.usd"]=value.str();
        }
        std::string ignored; telemetry_->emit_span(std::move(span),&ignored);
    }
    if(audit_) {
        AuditEvent event; event.timestamp=options_.now(); event.trace_id=request.trace_id;
        event.tenant_id=request.metadata.identity.tenant_id; event.task_id=request.metadata.identity.task_id;
        event.attempt=manifest.attempts.size(); event.component="llm_role_runtime";
        event.event_kind="invocation_finished"; event.capability_id=profile->role;
        event.capability_revision=profile->revision; event.outcome=result.ok?"succeeded":invocation_state_name(manifest.state);
        event.error_code=manifest.error_code; event.latency_ms=elapsed;
        event.payload={{"invocation_id",manifest.invocation_id},{"manifest_digest",encode(manifest).at("canonical_digest")},
            {"provider",manifest.provider},{"model",manifest.model},{"attempt_count",manifest.attempts.size()},
            {"output_digest",manifest.output_digest}};
        event.payload=redact_audit_payload(std::move(event.payload));
        event.payload_digest=audit_payload_digest(event.payload); audit_->write(event);
    }
    return result;
}

std::vector<StoredInvocation> RoleRuntime::reconcile_recoverable(
    std::string_view tenant_id, std::size_t limit) {
    auto records=store_->list_recoverable(tenant_id,limit);
    if(!options_.manual_review_uncertain_running) return records;
    for(auto& record : records) {
        if(record.manifest.state!=InvocationState::Running) continue;
        record.manifest.state=InvocationState::ManualReview;
        record.manifest.error_code="uncertain_provider_outcome_after_restart";
        record.manifest.error_message="process stopped while a provider invocation was running";
        record.manifest.finished_at=options_.now();
        const auto result=store_->update_invocation(record.manifest,record.revision);
        if(result.ok()) record.revision=result.revision;
    }
    return records;
}

}  // namespace agent_framework::llm_runtime
