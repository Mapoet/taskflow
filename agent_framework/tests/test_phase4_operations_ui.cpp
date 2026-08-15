#include <agent/ui/phase4_operations.hpp>
#include <agent/ui/presentation_model.hpp>
#include <agent/ui/tui_handler.hpp>
#include <agent/ui/ui_manager.hpp>
#include <agent/ui/interaction_source_adapters.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace agent_framework;

namespace {
template <typename Fn>
bool throws_invalid(Fn&& fn) {
    try { fn(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
} // namespace

int main() {
    const auto expected = Phase4OperationsProjection::demo_snapshot();
    json canonical = Phase4OperationsProjection::to_json(expected);
    const auto parsed = Phase4OperationsProjection::from_json(canonical);
    assert(parsed.snapshot_id == expected.snapshot_id);
    assert(parsed.stages.size() == 6U);
    assert(parsed.assurance.size() == 5U);
    assert(parsed.memory.size() == 5U);
    assert(parsed.invocations.size() == 4U);
    assert(parsed.hitl.size() == 1U);
    assert(parsed.task_closure_state == "manual_review");
    assert(!parsed.task_completion_verified && parsed.criteria_closed == 8);
    assert(canonical.dump().find("reasoning") == std::string::npos);
    assert(canonical.dump().find("api_key") == std::string::npos);
    assert(canonical.dump().find("raw_prompt") == std::string::npos);

    json with_private = canonical;
    with_private["raw_prompt"] = "never publish me";
    with_private["api_key"] = "secret";
    const json reprojected = Phase4OperationsProjection::to_json(
        Phase4OperationsProjection::from_json(with_private));
    assert(!reprojected.contains("raw_prompt"));
    assert(!reprojected.contains("api_key"));

    json bad_schema = canonical; bad_schema["schema_version"] = "phase4.operations.v999";
    assert(throws_invalid([&] { (void)Phase4OperationsProjection::from_json(bad_schema); }));
    json duplicate = canonical; duplicate["stages"].push_back(duplicate["stages"].front());
    assert(throws_invalid([&] { (void)Phase4OperationsProjection::from_json(duplicate); }));
    json bad_cost = canonical; bad_cost["invocations"][0]["cost_usd"] = -1.0;
    assert(throws_invalid([&] { (void)Phase4OperationsProjection::from_json(bad_cost); }));
    json unknown_evidence = canonical;
    unknown_evidence["stages"][0]["evidence_ids"].push_back("EV-UNKNOWN");
    assert(throws_invalid([&] { (void)Phase4OperationsProjection::from_json(unknown_evidence); }));

    std::ostringstream cli_output;
    auto cli = std::make_unique<CLIHandler>(cli_output);
    auto presentation = std::make_shared<UiPresentationModel>();
    auto tui = std::make_unique<TuiHandler>(presentation);
    TuiHandler* tui_ptr = tui.get();
    auto queue = std::make_shared<ThreadSafeQueue<StreamMessage>>();
    auto imgui = std::make_unique<ImGuiHandler>(queue, "default", presentation);
    auto connection = std::make_shared<WebConnectionInfo>();
    connection->session_id = "default"; connection->is_active = true;
    auto web = std::make_unique<WebHandler>("default", connection);
    WebHandler* web_ptr = web.get();

    UIManager manager;
    manager.register_cli_handler(std::move(cli));
    manager.register_handler(std::move(tui));
    manager.register_gui_handler(std::move(imgui));
    manager.register_web_connection("default", std::move(web));
    manager.publish_phase4_operations(expected);

    assert(cli_output.str().find("PHASE 4 OPERATIONS") != std::string::npos);
    assert(cli_output.str().find(expected.run_id) != std::string::npos);
    const auto ui = tui_ptr->presentation_snapshot();
    assert(ui.has_operations);
    assert(ui.operations.snapshot_id == expected.snapshot_id);
    assert(ui.operations.overall_status == expected.overall_status);

    std::string sse;
    assert(web_ptr->try_pop_sse_chunk(sse));
    assert(sse.find("phase4_operations") != std::string::npos);
    assert(sse.find(expected.snapshot_id) != std::string::npos);
    assert(sse.find("raw_prompt") == std::string::npos);

    const auto interactions=ui::project_interactions(expected,
        {"conversation-test","turn-test","message-test","Original test question"});
    manager.publish_interactions(interactions);
    assert(cli_output.str().find("[interactions]") != std::string::npos);
    assert(cli_output.str().find("orphan=0") != std::string::npos);
    assert(web_ptr->try_pop_sse_chunk(sse));
    assert(sse.find("interaction_snapshot") != std::string::npos);
    assert(sse.find("message:message-test") != std::string::npos);
    assert(sse.find("chain_of_thought") == std::string::npos);
    const auto interaction_ui=tui_ptr->presentation_snapshot();
    assert(interaction_ui.has_interactions);
    assert(interaction_ui.interactions.digest==interactions.digest);
    assert(!interaction_ui.selected_interaction_id.empty());
    const auto approval_node=std::find_if(interactions.nodes.begin(),interactions.nodes.end(),[](const auto& n){return n.kind==ui::InteractionNodeKind::Approval;});
    assert(approval_node!=interactions.nodes.end());assert(presentation->select_interaction(approval_node->node_id));
    assert(presentation->snapshot().selected_interaction_id==approval_node->node_id);

    const std::string text = Phase4OperationsProjection::render_text(expected, 96);
    assert(text.find("HITL pending") != std::string::npos);
    assert(text.find("BLOCKER") != std::string::npos);
    assert(text.find("UNVERIFIED") != std::string::npos);
    assert(text.find("authority=task_closure_controller") != std::string::npos);
    return 0;
}
