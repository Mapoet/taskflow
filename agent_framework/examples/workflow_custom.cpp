/** @file workflow_custom.cpp @brief Custom typed-any Workflow graph example. */

#include <workflow/nodeflow.hpp>

#include <any>
#include <iostream>
#include <string>
#include <unordered_map>

int main() {
    tf::Executor executor;
    workflow::GraphBuilder graph("custom_normalization_workflow");
    graph.create_any_source("Observation", {{"value", std::any{21}}});
    graph.create_any_node("Normalize", {{"Observation", "value"}},
        [](const std::unordered_map<std::string, std::any>& input) {
            return std::unordered_map<std::string, std::any>{
                {"normalized", std::any{std::any_cast<int>(input.at("value")) * 2}}};
        }, {"normalized"});
    int output = 0;
    graph.create_any_sink("Result", {{"Normalize", "normalized"}},
        [&](const std::unordered_map<std::string, std::any>& values) {
            output = std::any_cast<int>(values.at("normalized"));
        });
    graph.run_async(executor).get();
    if(output != 42) return 1;
    std::cout << "normalized=" << output << '\n';
    return 0;
}
