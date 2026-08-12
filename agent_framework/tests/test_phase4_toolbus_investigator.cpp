#include <cassert>
#include <memory>
#include <stdexcept>

#include "agent/planning/toolbus_investigator.hpp"

int main() {
    using namespace agent_framework;
    auto bus = std::make_shared<ToolBus>();
    ToolMeta read;
    read.name = "repo.inspect";
    read.schema = {{"type", "object"}};
    read.side_effect = ToolSideEffect::ReadOnly;
    bus->register_local_tool(read.name, [](const json&) {
        return json{{"path", "src/main.cpp"}, {"consumer", "test"}};
    }, read);
    ToolMeta write = read;
    write.name = "repo.write";
    write.side_effect = ToolSideEffect::Write;
    bus->register_local_tool(write.name, [](const json&) { return json{{"ok", true}}; }, write);

    planning::ToolBusInvestigatorOptions options;
    options.investigator_id = "repository";
    options.steps = {{"repo.inspect", json::object(), "repo://src/main.cpp",
                      {"public consumer exists"}, {}, "repository", "2099-01-01T00:00:00Z"}};
    options.now = [] { return "2026-08-12T00:00:00Z"; };
    planning::ToolBusInvestigator investigator(bus, options);
    planning::InvestigationRequest request;
    request.intake.metadata.identity.tenant_id = "tenant";
    request.intake.metadata.identity.task_id = "task";
    request.remaining_tool_calls = 1;
    std::string error;
    const auto evidence = investigator.investigate(request, &error);
    assert(error.empty() && evidence.size() == 1);
    assert(evidence.front().origin_kind == "repository");
    assert(!evidence.front().instruction_authority);
    assert(!evidence.front().content_digest.empty());
    request.remaining_tool_calls = 0;
    assert(investigator.investigate(request, &error).empty());
    assert(error.find("budget") != std::string::npos);

    options.steps.front().tool_name = "repo.write";
    bool rejected = false;
    try { planning::ToolBusInvestigator denied(bus, options); }
    catch(const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}
