#include "agent/ui/live_operations_projection.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "agent/contracts/contract.hpp"

namespace agent_framework {
namespace {

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &time);
#else
    gmtime_r(&time, &value);
#endif
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string digest(Phase4OperationsSnapshot snapshot) {
    snapshot.snapshot_id.clear();
    return contracts::canonical_digest(Phase4OperationsProjection::to_json(snapshot))
        .value_or("sha256:unavailable");
}

bool failed_result(const json& result) {
    return result.is_object() && (result.contains("error") ||
        (result.contains("ok") && result["ok"].is_boolean() && !result["ok"].get<bool>()));
}

OperationsStatus invocation_status(std::string_view type) {
    if(type == "failed" || type == "invocation_failed" ||
       type == "invocation_orphaned" || type == "orphan_input_unavailable")
        return OperationsStatus::Failed;
    if(type == "completed_candidate" || type == "invocation_completed_candidate" ||
       type == "effect_committed" || type == "verified" || type == "integrity_verified")
        return OperationsStatus::Passed;
    if(type == "manual_review" || type == "invocation_manual_review" ||
       type == "invocation_cancel_manual_review" || type == "unknown_effect")
        return OperationsStatus::Warning;
    if(type == "created" || type == "admitted" || type == "queued" || type == "leased" ||
       type == "running" || type == "progress" || type == "progressing" ||
       type == "checkpointed" || type == "awaiting_input" || type == "awaiting_approval" ||
       type == "cancelling" || type == "retrying" || type == "reconciling")
        return OperationsStatus::Running;
    return OperationsStatus::Unknown;
}

void project_activity_status(Phase4OperationsSnapshot& snapshot, OperationsStatus activity) {
    // Tool lifecycle is evidence about one invocation, never task closure.
    if(snapshot.task_completion_verified) return;
    if(activity == OperationsStatus::Failed || activity == OperationsStatus::Warning ||
       activity == OperationsStatus::Unknown)
        snapshot.overall_status = OperationsStatus::Warning;
    else
        snapshot.overall_status = OperationsStatus::Running;
}

}  // namespace

LiveOperationsProjection::LiveOperationsProjection(
    LiveOperationsIdentity identity, std::shared_ptr<SQLiteOperationsSnapshotStore> store,
    Publisher publisher)
    : store_(std::move(store)), publisher_(std::move(publisher)) {
    snapshot_.tenant_id = std::move(identity.tenant_id);
    snapshot_.run_id = std::move(identity.run_id);
    snapshot_.task_id = std::move(identity.task_id);
    snapshot_.conversation_id = std::move(identity.conversation_id);
    snapshot_.turn_id = std::move(identity.turn_id);
    snapshot_.summary = "Live workflow observation";
    snapshot_.live_certification = "live-unverified";
    initialize_locked();
}

LiveOperationsProjection::LiveOperationsProjection(
    Phase4OperationsSnapshot initial, std::shared_ptr<SQLiteOperationsSnapshotStore> store,
    Publisher publisher)
    : snapshot_(std::move(initial)), store_(std::move(store)), publisher_(std::move(publisher)) {
    initialize_locked();
}

void LiveOperationsProjection::initialize_locked() {
    if(snapshot_.tenant_id.empty() || snapshot_.run_id.empty() || snapshot_.task_id.empty())
        throw std::invalid_argument("live operations identity fields are required");
    auto source = std::find_if(snapshot_.source_revisions.begin(), snapshot_.source_revisions.end(),
        [](const auto& item) { return item.store == "tool_lifecycle"; });
    if(source != snapshot_.source_revisions.end()) revision_ = source->revision;
    if(snapshot_.updated_at.empty()) snapshot_.updated_at = timestamp();
    const auto compacted = Phase4OperationsProjection::compact_invocations(snapshot_);
    if(snapshot_.snapshot_id.empty() || compacted != 0) snapshot_.snapshot_id = digest(snapshot_);
}

void LiveOperationsProjection::observe_tool(const ToolExecutionEvent& event) {
    Phase4OperationsSnapshot published;
    Publisher publisher;
    {
        std::lock_guard lock(mutex_);
        auto invocation = std::find_if(snapshot_.invocations.begin(), snapshot_.invocations.end(),
            [&](const auto& item) { return item.id == event.tool_call_id; });
        if(invocation == snapshot_.invocations.end()) {
            snapshot_.invocations.push_back({event.tool_call_id, "tool", event.tool_name, "", "",
                "live-tool-observation", "none", 0, 0, 0.0, 0, OperationsStatus::Running});
            invocation = std::prev(snapshot_.invocations.end());
        }
        invocation->provider = event.tool_name;
        invocation->status = event.phase == ToolExecutionPhase::Started
            ? OperationsStatus::Running
            : (failed_result(event.result) ? OperationsStatus::Failed : OperationsStatus::Passed);

        ++revision_;
        snapshot_.updated_at = timestamp();
        project_activity_status(snapshot_, invocation->status);
        snapshot_.summary = event.phase == ToolExecutionPhase::Started
            ? "Tool running: " + event.tool_name
            : "Tool observation synchronized: " + event.tool_name;
        auto source = std::find_if(snapshot_.source_revisions.begin(), snapshot_.source_revisions.end(),
            [](const auto& item) { return item.store == "tool_lifecycle"; });
        OperationsSourceRevision next{"tool_lifecycle", snapshot_.run_id, revision_, ""};
        next.digest = contracts::canonical_digest({{"tool_call_id", event.tool_call_id},
            {"tool_name", event.tool_name}, {"phase", event.phase == ToolExecutionPhase::Started
                ? "started" : "completed"}, {"revision", revision_}}).value_or("sha256:unavailable");
        if(source == snapshot_.source_revisions.end()) snapshot_.source_revisions.push_back(next);
        else *source = std::move(next);
        Phase4OperationsProjection::compact_invocations(snapshot_);
        snapshot_.snapshot_id = digest(snapshot_);
        if(store_) {
            std::string error;
            if(!store_->save(snapshot_, &error))
                throw std::runtime_error("cannot persist live operations snapshot: " + error);
        }
        published = snapshot_;
        publisher = publisher_;
    }
    if(publisher) publisher(published);
}

Phase4OperationsSnapshot LiveOperationsProjection::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void LiveOperationsProjection::observe_invocation(const tool_runtime::InvocationEvent& event) {
    if(event.invocation_id.empty() || event.sequence == 0 ||
       event.durability != tool_runtime::InvocationEventDurability::Durable) return;
    Phase4OperationsSnapshot published;
    Publisher publisher;
    {
        std::lock_guard lock(mutex_);
        auto source = std::find_if(snapshot_.source_revisions.begin(), snapshot_.source_revisions.end(),
            [&](const auto& item) { return item.store == "tool_invocation_events" &&
                                           item.object_id == event.invocation_id; });
        if(source != snapshot_.source_revisions.end() && event.sequence <= source->revision) return;
        auto invocation = std::find_if(snapshot_.invocations.begin(), snapshot_.invocations.end(),
            [&](const auto& item) { return item.id == event.invocation_id; });
        const auto tool_name = event.payload.value("tool_name", std::string("durable-tool"));
        if(invocation == snapshot_.invocations.end()) {
            snapshot_.invocations.push_back({event.invocation_id, "tool", tool_name, "", "",
                "durable-tool-observation", "none", 0, 0, 0.0, 0, OperationsStatus::Running});
            invocation = std::prev(snapshot_.invocations.end());
        }
        invocation->provider = tool_name;
        const auto& type = event.event_type;
        invocation->status = invocation_status(type);
        snapshot_.updated_at = event.created_at.empty() ? timestamp() : event.created_at;
        project_activity_status(snapshot_, invocation->status);
        snapshot_.summary = "Tool lifecycle synchronized: " + type;
        if(event.payload.contains("fraction"))
            snapshot_.summary += " (" + std::to_string(event.payload.at("fraction").get<double>() * 100.0) + "%)";
        OperationsSourceRevision next{"tool_invocation_events", event.invocation_id, event.sequence, event.event_digest};
        if(source == snapshot_.source_revisions.end()) snapshot_.source_revisions.push_back(next); else *source = next;
        Phase4OperationsProjection::compact_invocations(snapshot_);
        snapshot_.snapshot_id = digest(snapshot_);
        if(store_) {
            std::string error;
            if(!store_->save(snapshot_, &error))
                throw std::runtime_error("cannot persist durable operations snapshot: " + error);
        }
        published = snapshot_;
        publisher = publisher_;
    }
    if(publisher) publisher(published);
}

void LiveOperationsProjection::observe_runtime(
    const conversation::RuntimeEventEnvelope& event) {
    if(event.sequence == 0 || event.durability != conversation::EventDurability::Durable)
        return;
    Phase4OperationsSnapshot published;
    Publisher publisher;
    {
        std::lock_guard lock(mutex_);
        auto source = std::find_if(snapshot_.source_revisions.begin(),
            snapshot_.source_revisions.end(), [](const auto& item) {
                return item.store == "conversation_harness_events";
            });
        if(source != snapshot_.source_revisions.end() &&
           event.sequence <= source->revision) return;

        snapshot_.tenant_id = event.tenant_id;
        snapshot_.run_id = event.run_id;
        snapshot_.conversation_id = event.conversation_id;
        snapshot_.turn_id = event.turn_id;
        snapshot_.updated_at = event.timestamp.empty() ? timestamp() : event.timestamp;
        snapshot_.task_completion_verified = false;
        snapshot_.completion_authority = "none";
        const auto type = event.event_type;
        if(type == "harness.stage_result") {
            const auto stage_id = event.payload.value("stage", std::string("unknown"));
            auto stage = std::find_if(snapshot_.stages.begin(), snapshot_.stages.end(),
                [&](const auto& value) { return value.id == stage_id; });
            if(stage == snapshot_.stages.end()) {
                snapshot_.stages.push_back({stage_id, stage_id, OperationsStatus::Running,
                    0, "harness", "", {}});
                stage = std::prev(snapshot_.stages.end());
            }
            const auto outcome = event.payload.value("outcome", std::string("failed"));
            stage->status = outcome == "succeeded" ? OperationsStatus::Passed
                : outcome == "awaiting_approval" ? OperationsStatus::Pending
                : outcome == "awaiting_external" ? OperationsStatus::Running
                : outcome == "needs_remediation" ? OperationsStatus::Warning
                : outcome == "rejected" ? OperationsStatus::Blocked
                : OperationsStatus::Failed;
            stage->revision = event.payload.value("checkpoint_revision",
                                                  event.sequence);
            stage->summary = outcome;
            if(event.payload.contains("output_digest") &&
               event.payload["output_digest"].is_string()) {
                const auto output_digest = event.payload["output_digest"].get<std::string>();
                const auto evidence_id = "harness-stage:" + stage_id + ":output";
                stage->evidence_ids = {evidence_id};
                auto evidence = std::find_if(snapshot_.evidence.begin(), snapshot_.evidence.end(),
                    [&](const auto& value) { return value.id == evidence_id; });
                OperationsEvidence projected{evidence_id,
                    "Committed output for Harness stage " + stage_id,
                    "stage_output", "conversation_harness_events", "committed",
                    snapshot_.updated_at, stage->status, output_digest};
                if(evidence == snapshot_.evidence.end())
                    snapshot_.evidence.push_back(std::move(projected));
                else
                    *evidence = std::move(projected);
            }
            const auto output = event.payload.value(
                "public_output", nlohmann::json::object());
            if(stage_id == "cognition" && output.is_object() &&
               output.value("schema", "") ==
                   "agent.lightweight_conversation_plan/v1") {
                ++snapshot_.plan_revision;
                snapshot_.criteria_total = output.value(
                    "acceptance_criteria", nlohmann::json::array()).size();
                snapshot_.criteria_closed = 0;
            }
            snapshot_.summary = "Harness stage synchronized: " + stage_id +
                                " (" + outcome + ")";
            project_activity_status(snapshot_, stage->status);
        } else if(type == "harness.harness_completed") {
            snapshot_.task_closure_state = "execution_completed_unverified";
            snapshot_.task_closure_reason =
                "structural harness completion; semantic closure not evaluated";
            snapshot_.overall_status = OperationsStatus::Running;
            snapshot_.summary =
                "Execution completed; authoritative task verification pending";
        } else if(type == "harness.completion_gate_failed" ||
                  type == "harness.stage_port_missing" ||
                  type == "harness.effect_unknown") {
            snapshot_.overall_status = OperationsStatus::Blocked;
            snapshot_.blocker = type;
            snapshot_.summary = "Harness blocked: " + type;
        } else if(type.rfind("harness.", 0) == 0) {
            snapshot_.overall_status = OperationsStatus::Running;
            snapshot_.summary = "Harness event synchronized: " + type.substr(8);
        }

        OperationsSourceRevision next{"conversation_harness_events",
            event.conversation_id, event.sequence, event.digest};
        if(source == snapshot_.source_revisions.end())
            snapshot_.source_revisions.push_back(next);
        else *source = std::move(next);
        Phase4OperationsProjection::compact_invocations(snapshot_);
        snapshot_.snapshot_id = digest(snapshot_);
        if(store_) {
            std::string error;
            if(!store_->save(snapshot_, &error))
                throw std::runtime_error(
                    "cannot persist runtime operations snapshot: " + error);
        }
        published = snapshot_;
        publisher = publisher_;
    }
    if(publisher) publisher(published);
}

std::size_t LiveOperationsProjection::consume_invocations(
    tool_runtime::InvocationEventSubscription& subscription, std::chrono::milliseconds timeout,
    std::size_t limit) {
    std::size_t consumed = 0;
    while(consumed < limit) {
        tool_runtime::InvocationEvent event;
        const auto status = subscription.next(event, consumed == 0 ? timeout : std::chrono::milliseconds(0));
        if(status != tool_runtime::InvocationSubscriptionRead::Event) break;
        observe_invocation(event);
        ++consumed;
    }
    return consumed;
}

}  // namespace agent_framework
