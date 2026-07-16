/** @file tool_integration.cpp @brief ToolBus schema, hook, and invocation example. */

#include <agent/toolbus/toolbus.hpp>

#include <iostream>

using namespace agent_framework;

int main() {
    ToolBus bus;
    ToolMeta meta;
    meta.name = "vector_norm_squared";
    meta.description = "Return x*x + y*y";
    meta.side_effect = ToolSideEffect::ReadOnly;
    meta.schema = nlohmann::json::parse(R"({
      "type":"object",
      "properties":{"x":{"type":"number"},"y":{"type":"number"}},
      "required":["x","y"]
    })");
    bus.register_local_tool(meta.name, [](const nlohmann::json& input) {
        const double x = input.at("x").get<double>();
        const double y = input.at("y").get<double>();
        return nlohmann::json{{"result", x * x + y * y}};
    }, meta);

    auto result = bus.call_tool(meta.name, {{"x", 3.0}, {"y", 4.0}}).get();
    if(result.value("result", 0.0) != 25.0) return 1;
    std::cout << result.dump() << '\n';
    return 0;
}
