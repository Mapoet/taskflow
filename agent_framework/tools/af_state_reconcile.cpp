#include <agent/conversation/store.hpp>
#include <agent/harness/store.hpp>
#include <agent/recovery/system_state_reconciler.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

using namespace agent_framework;

namespace {

std::string required(int& index, int argc, char** argv, std::string_view option) {
    if (++index >= argc) throw std::invalid_argument(std::string(option) + " requires a value");
    return argv[index];
}

} // namespace

int main(int argc, char** argv) try {
    std::string conversation_db;
    std::string harness_db;
    std::string tenant;
    std::string conversation;
    bool apply = false;
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "--conversation-db") conversation_db = required(i, argc, argv, option);
        else if (option == "--harness-db") harness_db = required(i, argc, argv, option);
        else if (option == "--tenant") tenant = required(i, argc, argv, option);
        else if (option == "--conversation") conversation = required(i, argc, argv, option);
        else if (option == "--apply") apply = true;
        else throw std::invalid_argument("unknown option: " + option);
    }
    if (conversation_db.empty() || harness_db.empty() || tenant.empty() || conversation.empty())
        throw std::invalid_argument(
            "--conversation-db, --harness-db, --tenant and --conversation are required");

    conversation::SQLiteConversationStore conversations(conversation_db);
    harness::SQLiteHarnessStore harnesses(harness_db);
    recovery::SystemStateReconciler reconciler(conversations, harnesses);
    const auto plan = reconciler.scan({{tenant, conversation}, 4096});
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& finding : plan.findings) {
        findings.push_back({{"code", finding.code},
                            {"disposition", recovery::name(finding.disposition)},
                            {"turn_id", finding.turn_id},
                            {"harness_id", finding.harness_id},
                            {"turn_revision", finding.turn_revision},
                            {"harness_revision", finding.harness_revision},
                            {"reason", finding.reason}});
    }
    nlohmann::json output{{"schema", "agent.system_reconciliation_report/v1"},
                          {"dry_run", !apply},
                          {"plan_digest", plan.digest},
                          {"finding_count", plan.findings.size()},
                          {"findings", std::move(findings)}};
    if (apply) {
        const auto result = reconciler.apply(plan, false);
        output["apply"] = {{"inspected", result.inspected},
                           {"changed", result.changed},
                           {"conflicts", result.conflicts},
                           {"manual_review", result.manual_review},
                           {"errors", result.errors}};
        std::cout << output.dump(2) << '\n';
        return result.errors.empty() && result.conflicts == 0 ? 0 : 2;
    }
    std::cout << output.dump(2) << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "af_state_reconcile: " << error.what() << '\n';
    return 2;
}
