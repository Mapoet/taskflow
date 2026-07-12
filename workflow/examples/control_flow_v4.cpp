#include <workflow/nodeflow.hpp>

#include <any>
#include <iostream>

int main() {
  tf::Executor executor;
  workflow::GraphBuilder builder("control_flow_v4");
  builder.create_any_source("initial", {{"value", 0}});

  workflow::LoopOptions options;
  options.max_iterations = 10;
  options.feedback = {{"value", "value"}};
  builder.create_loop(
    "increment",
    {{"initial", "value"}},
    [](const workflow::ValueMap& inputs, const workflow::IterationContext&) {
      return workflow::ValueMap{{"value", std::any_cast<int>(inputs.at("value")) + 1}};
    },
    [](const workflow::ValueMap& outputs, const workflow::IterationContext&) {
      return std::any_cast<int>(outputs.at("value")) >= 3
        ? workflow::LoopDecision::Exit : workflow::LoopDecision::Continue;
    },
    {},
    {"value"},
    options);

  builder.create_any_sink(
    "result",
    {{"increment", "value"}},
    [](const workflow::ValueMap& outputs) {
      std::cout << "value=" << std::any_cast<int>(outputs.at("value")) << '\n';
    });
  builder.run(executor);
}
