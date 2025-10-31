#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>

int main() {
  namespace wf = workflow;

  tf::Executor executor;
  wf::GraphBuilder builder("loop_only");

  int counter = 0;

  // Build loop body as a subgraph using declarative API
  auto loop_body_task = builder.create_subgraph("LoopBody", [&counter](wf::GraphBuilder& gb){
    // Simple trigger source (dummy)
    auto [trigger, tSrc] = gb.create_typed_source(
      "loop_trigger",
      std::make_tuple(0),
      std::vector<std::string>{"trigger"}
    );

    // Process: print and increment counter
    auto [process, tProc] = gb.create_typed_node<int>(
      "loop_iteration",
      {{"loop_trigger", "trigger"}},
      [&counter](const std::tuple<int>&) {
        std::cout << "Loop iteration: counter = " << counter << "\n";
        ++counter;
        return std::make_tuple(0);
      },
      {"result"}
    );

    // Sink to complete
    auto [sink, tSink] = gb.create_any_sink(
      "loop_complete",
      {{"loop_iteration", "result"}}
    );
  });

  // Optional exit action subgraph
  auto loop_exit_task = builder.create_subgraph("LoopExit", [](wf::GraphBuilder& gb){
    gb.taskflow().emplace([](){ std::cout << "Loop exit\n"; }).name("LoopExit_print");
  });

  // Loop: continue while counter < 5
  builder.create_loop_decl(
    "Loop",
    loop_body_task,
    [&counter]() -> int { 
      return (counter < 5) ? 0 : 1; 
    },
    loop_exit_task
  );

  std::cout << "=== Running loop_only example ===\n";
  builder.run(executor);
  std::cout << "=== Done ===\n";
  return 0;
}


