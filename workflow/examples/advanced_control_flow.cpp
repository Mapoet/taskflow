// Advanced Control Flow Example:
// Demonstrates condition, multi-condition, pipeline, and loop nodes
//
// Graph structure:
//   A (source) -> B (condition) -> C or D (branches)
//   E (source) -> F (multi-condition) -> G, H (parallel branches)
//   I -> Pipeline -> J
//   K -> Loop -> L

#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <array>

int main() {
  namespace wf = workflow;
  tf::Executor executor;
  wf::GraphBuilder builder("advanced_control_flow");

  std::cout << "=== Advanced Control Flow Example ===\n\n";
#if 1
  // ==========================================================================
  // Example 1: Condition Node (if-else branching)
  // ==========================================================================
  std::cout << "1. Condition Node (if-else):\n";

  auto [A, tA] = builder.create_typed_source("A",
    std::make_tuple(42),
    {"value"}
  );

  // (Use declarative condition below; do not create a separate B node)

  // Build branches as subgraphs using declarative API
  auto C_task = builder.create_subgraph("C", [&](wf::GraphBuilder& gb){
    auto [CS, tCS] = gb.create_typed_source("C_src", std::make_tuple(100.0), std::vector<std::string>{"x"});
    auto [CN, tCN] = gb.create_typed_node<double>("C_proc", {{"C_src","x"}}, [](const std::tuple<double>& in){
      std::cout << "  -> Even branch (C)\n";
      return std::make_tuple(std::get<0>(in) * 2.0);
    }, std::vector<std::string>{"y"});
    auto [CK, tCK] = gb.create_any_sink("C_sink", {{"C_proc","y"}});
  });

  auto D_task = builder.create_subgraph("D", [&](wf::GraphBuilder& gb){
    auto [DS, tDS] = gb.create_typed_source("D_src", std::make_tuple(200.0), std::vector<std::string>{"x"});
    auto [DN, tDN] = gb.create_typed_node<double>("D_proc", {{"D_src","x"}}, [](const std::tuple<double>& in){
      std::cout << "  -> Odd branch (D)\n";
      return std::make_tuple(std::get<0>(in) * 3.0);
    }, std::vector<std::string>{"y"});
    auto [DK, tDK] = gb.create_any_sink("D_sink", {{"D_proc","y"}});
  });

  // Declarative condition wiring
  builder.create_condition_decl("B",
    std::vector<std::string>{"A"},
    [](){ return 0; },
    std::vector<tf::Task>{C_task, D_task}
  );

  std::cout << "  A -> B (condition) -> C or D\n";

  // ==========================================================================
  // Example 2: Multi-Condition Node (parallel branching)
  // ==========================================================================
  std::cout << "\n2. Multi-Condition Node (parallel branches):\n";

  auto [E, tE] = builder.create_typed_source("E",
    std::make_tuple(100),
    {"data"}
  );

  // (Use declarative multi-condition below; do not create a separate F node)

  // Multiple branches as subgraphs
  auto G_task = builder.create_subgraph("G", [&](wf::GraphBuilder& gb){
    auto [S, tS] = gb.create_typed_source("Sg", std::make_tuple(1.0), std::vector<std::string>{"v"});
    auto [K, tK] = gb.create_any_sink("Kg", {{"Sg","v"}});
    std::cout << "  -> Branch G (executed)\n";
  });
  auto H_task = builder.create_subgraph("H", [&](wf::GraphBuilder& gb){
    auto [S, tS] = gb.create_typed_source("Sh", std::make_tuple(2.0), std::vector<std::string>{"v"});
    auto [K, tK] = gb.create_any_sink("Kh", {{"Sh","v"}});
    std::cout << "  -> Branch H (not executed)\n";
  });
  auto I_task = builder.create_subgraph("I", [&](wf::GraphBuilder& gb){
    auto [S, tS] = gb.create_typed_source("Si", std::make_tuple(3.0), std::vector<std::string>{"v"});
    auto [K, tK] = gb.create_any_sink("Ki", {{"Si","v"}});
    std::cout << "  -> Branch I (executed)\n";
  });
  // Declarative multi-condition wiring
  builder.create_multi_condition_decl("F",
    std::vector<std::string>{"E"},
    [](){ return tf::SmallVector<int>{0,2}; },
    std::vector<tf::Task>{G_task, H_task, I_task}
  );

  std::cout << "  E -> F (multi-condition) -> G, I (parallel)\n";

  // ==========================================================================
  // Example 3: Pipeline Node
  // ==========================================================================
  std::cout << "\n3. Pipeline Node:\n";

  std::array<size_t, 4> buffer;

  auto [Pipeline, tPipeline] = builder.create_pipeline_node("Pipeline",
    4,  // 4 parallel lines
    tf::Pipe{tf::PipeType::SERIAL, [&buffer](tf::Pipeflow& pf) {
      if (pf.token() == 5) {
        pf.stop();
      } else {
        printf("  Stage 1: token=%zu, line=%zu\n", pf.token(), pf.line());
        buffer[pf.line()] = pf.token();
      }
    }},
    tf::Pipe{tf::PipeType::PARALLEL, [&buffer](tf::Pipeflow& pf) {
      printf("  Stage 2: token=%zu, line=%zu, buffer[%zu]=%zu\n", 
             pf.token(), pf.line(), pf.line(), buffer[pf.line()]);
      buffer[pf.line()] = buffer[pf.line()] + 1;
    }},
    tf::Pipe{tf::PipeType::SERIAL, [&buffer](tf::Pipeflow& pf) {
      printf("  Stage 3: token=%zu, line=%zu, buffer[%zu]=%zu\n",
             pf.token(), pf.line(), pf.line(), buffer[pf.line()]);
    }}
  );

  std::cout << "  Pipeline with 3 stages, 4 parallel lines\n";
#endif
  // ==========================================================================
  // Example 4: Loop Node
  // ==========================================================================
  std::cout << "\n4. Loop Node:\n";

  int counter = 0;

  // Build loop body using create_subtask so the subgraph is built & run each iteration
  auto loop_body_task = builder.create_subtask("LoopBody", [&counter](wf::GraphBuilder& gb){
    auto [dummy_src, tSrc] = gb.create_typed_source(
      "loop_trigger",
      std::make_tuple(counter),
      std::vector<std::string>{"trigger"}
    );

    auto [process, tProc] = gb.create_typed_node<int>(
      "loop_iteration",
      {{"loop_trigger", "trigger"}},
      [&counter](const std::tuple<int>&) {
        ++counter;
        return std::make_tuple(counter);
      },
      std::vector<std::string>{"result"}
    );

    auto [sink, tSink] = gb.create_any_sink(
      "loop_complete",
      {{"loop_iteration", "result"}}
    );
  });

  // Optional exit action subgraph using declarative API
  auto loop_exit_task = builder.create_subgraph("LoopExit", [](wf::GraphBuilder& gb){
    // Create a simple source and sink for exit message
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

  // Declarative loop: condition 0 loops back to body, non-zero goes to exit
  builder.create_loop_decl(
    "Loop",
    std::vector<std::string>{"A"},
    loop_body_task,
    [&counter]() -> int { 
      // Only read counter, don't modify it (modification happens in loop body)
      int result = (counter < 5) ? 0 : 1;
      return result;
    },
    loop_exit_task
  );

  std::cout << "  Loop structure (decl): body -> condition -> (body if 0, exit if non-zero)\n";

  // ==========================================================================
  // Execute
  // ==========================================================================
  
  std::cout << "\n=== Executing workflow ===\n\n";
  
  builder.run(executor);
  
  std::cout << "\n=== Workflow completed ===\n";
  builder.dump(std::cout);
  
  return 0;
}

