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

}  // namespace

LiveOperationsProjection::LiveOperationsProjection(
    LiveOperationsIdentity identity, std::shared_ptr<SQLiteOperationsSnapshotStore> store,
    Publisher publisher)
    : store_(std::move(store)), publisher_(std::move(publisher)) {
    snapshot_.tenant_id = std::move(identity.tenant_id);
    snapshot_.run_id = std::move(identity.run_id);
    snapshot_.task_id = std::move(identity.task_id);
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
    if(snapshot_.snapshot_id.empty()) snapshot_.snapshot_id = digest(snapshot_);
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
        snapshot_.overall_status = invocation->status == OperationsStatus::Failed
            ? OperationsStatus::Warning : OperationsStatus::Running;
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
            [](const auto& item) { return item.store == "tool_invocation_events"; });
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
        if(type.find("failed") != std::string::npos || type.find("orphan") != std::string::npos)
            invocation->status = OperationsStatus::Failed;
        else if(type.find("completed") != std::string::npos || type.find("verified") != std::string::npos ||
                type.find("effect_committed") != std::string::npos)
            invocation->status = OperationsStatus::Passed;
        else if(type.find("manual_review") != std::string::npos || type.find("unknown_effect") != std::string::npos)
            invocation->status = OperationsStatus::Warning;
        else invocation->status = OperationsStatus::Running;
        snapshot_.updated_at = event.created_at.empty() ? timestamp() : event.created_at;
        snapshot_.overall_status = invocation->status == OperationsStatus::Failed ? OperationsStatus::Warning
                                  : invocation->status == OperationsStatus::Passed ? OperationsStatus::Passed
                                                                                  : invocation->status;
        snapshot_.summary = "Tool lifecycle synchronized: " + type;
        if(event.payload.contains("fraction"))
            snapshot_.summary += " (" + std::to_string(event.payload.at("fraction").get<double>() * 100.0) + "%)";
        OperationsSourceRevision next{"tool_invocation_events", event.invocation_id, event.sequence, event.event_digest};
        if(source == snapshot_.source_revisions.end()) snapshot_.source_revisions.push_back(next); else *source = next;
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
