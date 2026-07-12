#include <workflow/nodeflow.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_cardinality(std::size_t target) {
  tf::Executor executor(2);
  workflow::GraphBuilder builder("loop_cardinality");
  builder.create_any_source("initial", {{"value", 0}});

  std::size_t body_calls = 0;
  std::size_t condition_calls = 0;
  std::size_t exit_calls = 0;
  workflow::LoopOptions options;
  options.max_iterations = target;
  options.feedback = {{"value", "value"}};

  auto [loop, task] = builder.create_loop(
    "loop",
    {{"initial", "value"}},
    [&body_calls](const workflow::ValueMap& inputs, const workflow::IterationContext& context) {
      assert(context.iteration == body_calls);
      ++body_calls;
      return workflow::ValueMap{{"value", std::any_cast<int>(inputs.at("value")) + 1}};
    },
    [&condition_calls, target](const workflow::ValueMap& outputs,
                               const workflow::IterationContext&) {
      ++condition_calls;
      return static_cast<std::size_t>(std::any_cast<int>(outputs.at("value"))) >= target
        ? workflow::LoopDecision::Exit : workflow::LoopDecision::Continue;
    },
    [&exit_calls](const workflow::ValueMap& outputs, const workflow::IterationContext&) {
      ++exit_calls;
      return outputs;
    },
    {"value"},
    options);
  (void)task;

  int observed = -1;
  builder.create_any_sink("sink", {{"loop", "value"}}, [&](const workflow::ValueMap& outputs) {
    observed = std::any_cast<int>(outputs.at("value"));
  });
  builder.run(executor);

  assert(body_calls == target);
  assert(condition_calls == target);
  assert(exit_calls == 1);
  assert(observed == static_cast<int>(target));
  assert(loop->last_result().iterations == target);
  const auto expected_status = target == 0
    ? workflow::LoopStatus::MaxIterations : workflow::LoopStatus::Completed;
  assert(loop->last_result().status == expected_status);
}

void test_rerun_and_errors() {
  tf::Executor executor(4);
  workflow::GraphBuilder builder("loop_rerun");
  builder.create_any_source("initial", {{"value", 0}});
  workflow::LoopOptions options;
  options.max_iterations = 2;
  options.feedback = {{"value", "value"}};
  auto [loop, _] = builder.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap& inputs, const workflow::IterationContext&) {
      return workflow::ValueMap{{"value", std::any_cast<int>(inputs.at("value")) + 1}};
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::LoopDecision::Continue;
    },
    {}, {"value"}, options);
  for (int i = 0; i < 3; ++i) {
    builder.run(executor);
    assert(std::any_cast<int>(builder.get_latest_output("loop", "value")) == 2);
    assert(loop->last_result().status == workflow::LoopStatus::MaxIterations);
  }

  workflow::GraphBuilder errors("loop_errors");
  errors.create_any_source("initial", {{"value", 0}});
  auto [error_loop, __] = errors.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap&, const workflow::IterationContext&) -> workflow::ValueMap {
      throw std::runtime_error("body failed");
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::LoopDecision::Exit;
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::ValueMap{{"value", -1}};
    },
    {"value"});
  errors.run(executor);
  assert(error_loop->last_result().status == workflow::LoopStatus::BodyError);
  assert(error_loop->last_result().error == "body failed");
}

void test_cancel_and_deadline() {
  tf::Executor executor(2);
  auto cancelled = std::make_shared<std::atomic_bool>(true);
  workflow::GraphBuilder builder("cancelled");
  builder.create_any_source("initial", {{"value", 7}});
  workflow::LoopOptions options;
  options.cancel_requested = cancelled;
  auto [loop, _] = builder.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::ValueMap{{"value", 0}};
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::LoopDecision::Continue;
    }, {}, {"value"}, options);
  builder.run(executor);
  assert(loop->last_result().status == workflow::LoopStatus::Cancelled);
  assert(std::any_cast<int>(builder.get_latest_output("loop", "value")) == 7);

  workflow::GraphBuilder deadline("deadline");
  deadline.create_any_source("initial", {{"value", 9}});
  workflow::LoopOptions deadline_options;
  deadline_options.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
  auto [deadline_loop, __] = deadline.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::ValueMap{{"value", 0}};
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::LoopDecision::Continue;
    }, {}, {"value"}, deadline_options);
  deadline.run(executor);
  assert(deadline_loop->last_result().status == workflow::LoopStatus::DeadlineExceeded);
}

void test_condition_and_exit_errors() {
  tf::Executor executor(2);
  workflow::GraphBuilder condition_graph("condition_error");
  condition_graph.create_any_source("initial", {{"value", 1}});
  auto [condition_loop, _] = condition_graph.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap& inputs, const workflow::IterationContext&) {
      return inputs;
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) -> workflow::LoopDecision {
      throw std::runtime_error("condition failed");
    }, {}, {"value"});
  condition_graph.run(executor);
  assert(condition_loop->last_result().status == workflow::LoopStatus::ConditionError);
  assert(condition_loop->last_result().error == "condition failed");

  workflow::GraphBuilder exit_graph("exit_error");
  exit_graph.create_any_source("initial", {{"value", 1}});
  auto [exit_loop, __] = exit_graph.create_loop(
    "loop", {{"initial", "value"}},
    [](const workflow::ValueMap& inputs, const workflow::IterationContext&) {
      return inputs;
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) {
      return workflow::LoopDecision::Exit;
    },
    [](const workflow::ValueMap&, const workflow::IterationContext&) -> workflow::ValueMap {
      throw std::runtime_error("exit failed");
    }, {"value"});
  exit_graph.run(executor);
  assert(exit_loop->last_result().status == workflow::LoopStatus::ExitError);
  assert(exit_loop->last_result().error == "exit failed");
  assert(std::any_cast<int>(exit_graph.get_latest_output("loop", "value")) == 1);
}

}  // namespace

int main() {
  for (std::size_t count : {0U, 1U, 2U, 10U, 1000U}) {
    test_cardinality(count);
  }
  test_rerun_and_errors();
  test_cancel_and_deadline();
  test_condition_and_exit_errors();
}
