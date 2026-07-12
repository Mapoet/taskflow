#include <workflow/nodeflow.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

int main() {
  static_assert(std::is_move_constructible_v<workflow::GraphBuilder>);
  static_assert(std::is_move_assignable_v<workflow::GraphBuilder>);
  tf::Executor executor(2);
  workflow::GraphBuilder builder("runtime_slots");
  builder.create_any_source("source", {{"value", 1}});

  std::atomic_int node_calls {0};
  builder.create_any_node(
    "increment",
    {{"source", "value"}},
    [&node_calls](const auto& inputs) {
      ++node_calls;
      return std::unordered_map<std::string, std::any>{
        {"result", std::any{std::any_cast<int>(inputs.at("value")) + 1}}
      };
    },
    {"result"}
  );

  std::vector<int> observed;
  builder.create_any_sink(
    "sink",
    {{"increment", "result"}},
    [&observed](const auto& inputs) {
      observed.push_back(std::any_cast<int>(inputs.at("result")));
    }
  );

  for (int i = 0; i < 3; ++i) {
    builder.run(executor);
  }
  assert((observed == std::vector<int>{2, 2, 2}));
  assert(node_calls == 3);
  assert(std::any_cast<int>(builder.get_latest_output("increment", "result")) == 2);

  workflow::GraphBuilder guarded("run_guard");
  guarded.create_any_source("source", {{"value", 1}});
  guarded.create_any_node(
    "slow",
    {{"source", "value"}},
    [](const auto& inputs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      return std::unordered_map<std::string, std::any>{{"result", inputs.at("value")}};
    },
    {"result"}
  );
  auto first = guarded.run_async(executor);
  bool rejected = false;
  try {
    (void)guarded.run_async(executor);
  } catch (const std::logic_error&) {
    rejected = true;
  }
  assert(rejected);
  first.wait();
  guarded.run(executor);
}
