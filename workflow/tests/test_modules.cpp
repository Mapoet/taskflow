#include <workflow/nodeflow.hpp>

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

workflow::OutputBindings arithmetic_module(
    workflow::GraphBuilder& nested,
    const workflow::ValueMap& inputs,
    const workflow::RunContext& context) {
  assert(!context.run_id.empty());
  assert(!context.parent_run_id.empty());
  nested.create_any_source("inputs", {
    {"a", inputs.at("a")},
    {"b", inputs.at("b")}
  });
  nested.create_any_node(
    "arithmetic",
    {{"inputs", "a"}, {"inputs", "b"}},
    [](const workflow::ValueMap& values) {
      const int a = std::any_cast<int>(values.at("a"));
      const int b = std::any_cast<int>(values.at("b"));
      return workflow::ValueMap{{"sum", a + b}, {"product", a * b}};
    },
    {"sum", "product"});
  return {
    {"sum", {"arithmetic", "sum"}},
    {"product", {"arithmetic", "product"}}
  };
}

}  // namespace

int main() {
  tf::Executor executor(4);
  workflow::GraphBuilder builder("modules");
  builder.create_any_source("source_a", {{"a", 3}});
  builder.create_any_source("source_b", {{"b", 4}});

  auto module = std::make_shared<workflow::SubflowModule>(arithmetic_module);
  builder.create_subgraph_module(
    "static_arithmetic",
    {{"source_a", "a"}, {"source_b", "b"}},
    module,
    {"sum", "product"});

  int sink_calls = 0;
  builder.create_any_sink(
    "sink",
    {{"static_arithmetic", "sum"}, {"static_arithmetic", "product"}},
    [&sink_calls](const workflow::ValueMap& outputs) {
      assert(std::any_cast<int>(outputs.at("sum")) == 7);
      assert(std::any_cast<int>(outputs.at("product")) == 12);
      ++sink_calls;
    });

  for (int i = 0; i < 10; ++i) {
    builder.run(executor);
  }
  assert(sink_calls == 10);

  workflow::GraphBuilder missing("missing_binding");
  missing.create_any_source("source", {{"a", 1}});
  missing.create_subtask_module(
    "bad",
    {{"source", "a"}},
    [](workflow::GraphBuilder& nested, const workflow::ValueMap& inputs,
       const workflow::RunContext&) {
      nested.create_any_source("value", {{"a", inputs.at("a")}});
      return workflow::OutputBindings{};
    },
    {"required"});
  bool missing_rejected = false;
  try {
    missing.run(executor);
  } catch (const std::invalid_argument& e) {
    missing_rejected = std::string(e.what()).find("Missing subflow output binding") != std::string::npos;
  }
  assert(missing_rejected);

  workflow::GraphBuilder depth("depth");
  depth.create_any_source("source", {{"value", 1}});
  workflow::SubflowOptions options;
  options.max_depth = 0;
  depth.create_subtask_module(
    "too_deep",
    {{"source", "value"}},
    [](workflow::GraphBuilder&, const workflow::ValueMap&, const workflow::RunContext&) {
      return workflow::OutputBindings{};
    },
    {},
    options);
  bool depth_rejected = false;
  try {
    depth.run(executor);
  } catch (const std::runtime_error& e) {
    depth_rejected = std::string(e.what()).find("Maximum subflow depth") != std::string::npos;
  }
  assert(depth_rejected);

  workflow::GraphBuilder nested("nested_context");
  auto inherited_cancel = std::make_shared<std::atomic_bool>(false);
  const auto inherited_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  nested.configure_run_context(inherited_cancel, inherited_deadline, 4);
  nested.create_any_source("source", {{"value", 11}});
  nested.create_subtask_module(
    "parent",
    {{"source", "value"}},
    [inherited_cancel, inherited_deadline](workflow::GraphBuilder& parent,
       const workflow::ValueMap& inputs, const workflow::RunContext& parent_context) {
      assert(parent_context.depth == 1);
      assert(parent_context.cancel_requested == inherited_cancel);
      assert(parent_context.deadline == inherited_deadline);
      parent.create_any_source("parent_input", {{"value", inputs.at("value")}});
      parent.create_subtask_module(
        "child",
        {{"parent_input", "value"}},
        [](workflow::GraphBuilder& child, const workflow::ValueMap& child_inputs,
           const workflow::RunContext& child_context) {
          assert(child_context.depth == 2);
          child.create_any_source("child_input", {{"value", child_inputs.at("value")}});
          child.create_subtask_module(
            "grandchild",
            {{"child_input", "value"}},
            [](workflow::GraphBuilder& grandchild, const workflow::ValueMap& grandchild_inputs,
               const workflow::RunContext& grandchild_context) {
              assert(grandchild_context.depth == 3);
              grandchild.create_any_source(
                "result", {{"value", grandchild_inputs.at("value")}});
              return workflow::OutputBindings{{"value", {"result", "value"}}};
            }, {"value"});
          return workflow::OutputBindings{{"value", {"grandchild", "value"}}};
        }, {"value"});
      return workflow::OutputBindings{{"value", {"child", "value"}}};
    }, {"value"});
  nested.run(executor);
  assert(std::any_cast<int>(nested.get_latest_output("parent", "value")) == 11);
}
