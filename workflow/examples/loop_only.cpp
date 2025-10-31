#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>

int main() {
  namespace wf = workflow;

  tf::Executor executor;
  wf::GraphBuilder builder("loop_only");

  int counter = 0;

  // Input trigger
  auto input_task = builder.create_typed_source("Input", std::make_tuple(0), std::vector<std::string>{"input"});

  // Loop body: use create_subtask to build & run a fresh subgraph each iteration
  auto loop_body_task = builder.create_subtask("LoopBody", [&counter](wf::GraphBuilder& gb){
    auto [trigger, tSrc] = gb.create_typed_source(
      "loop_trigger",
      std::make_tuple(counter),
      std::vector<std::string>{"trigger"}
    );
    auto [process, tProc] = gb.create_typed_node<int>(
      "loop_iteration",
      {{"loop_trigger", "trigger"}},
      [&counter](const std::tuple<int>& in) {
        auto out = std::get<0>(in) + 1;
        counter = out;
        return std::make_tuple(out);
      },
      {"result"}
    );
    auto [sink, tSink] = gb.create_any_sink(
      "loop_complete",
      {{"loop_iteration", "result"}}
    );
  });

  // Optional exit action subgraph
  auto loop_exit_task = builder.create_subgraph("LoopExit", [](wf::GraphBuilder& gb){
    auto [exit_src, tExitSrc] = gb.create_typed_source("exit_msg",
      std::make_tuple(0),
      std::vector<std::string>{"msg"}
    );

    auto [exit_proc, tExitProc] = gb.create_typed_node<int>("exit_print",
      {{"exit_msg", "msg"}},
      [](const std::tuple<int>&) {
        return std::make_tuple(0);
      },
      {"done"}
    );

    auto [exit_sink, tExitSink] = gb.create_any_sink("exit_sink",
      {{"exit_print", "done"}}
    );
  });

  // Loop: continue while counter < 5
  builder.create_loop_decl(
    "Loop",
    {"Input"},
    loop_body_task,
    [&counter]() -> int { 
      return (counter < 5) ? 0 : 1; 
    },
    loop_exit_task
  );

  std::cout << "=== Running loop_only example ===\n";
  builder.run(executor);
  builder.dump(std::cout);
  std::cout << "=== Done ===\n";
  return 0;
}


