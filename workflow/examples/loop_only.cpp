#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>

int main() {
  namespace wf = workflow;

  tf::Executor executor;
  wf::GraphBuilder builder("loop_only");
  int counter = 0;

  // Input trigger
  auto input_task = builder.create_typed_source("Input", std::make_tuple(counter), std::vector<std::string>{"input"});
#if 1

  // Loop: continue while counter < 5
  // Using declarative API with string-keyed inputs
  builder.create_loop_decl(
    "Loop",
    {{"Input", "input"}},  // input_specs: from Input's "input" output
    [&counter](wf::GraphBuilder& gb, const std::unordered_map<std::string, std::any>& inputs) {
      // Build loop body - note: counter is captured by reference, will read current value at execution time
      auto current_count = counter;  // Capture current value when building graph
      auto [trigger, tSrc] = gb.create_typed_source(
        "loop_trigger",
        std::make_tuple(std::any_cast<int>(inputs.at("input"))),
        std::vector<std::string>{"trigger"}
      );
      auto [process, tProc] = gb.create_typed_node<int>(
        "loop_iteration",
        {{"loop_trigger", "trigger"}},
        [&counter](const std::tuple<int>& in) {
          auto old_counter = std::get<0>(in);
          auto out = old_counter + 1;
          counter = out;  // Update counter for next iteration
          std::cout << "  Process: in=" << std::get<0>(in) << ", old_counter=" << old_counter << ", new_counter=" << counter << "\n";
          return std::make_tuple(out);
        },
        {"result"}
      );
      auto [sink, tSink] = gb.create_any_sink(
        "loop_complete",
        {{"loop_iteration", "result"}},
        [](const std::unordered_map<std::string, std::any>& values) {
          if (values.find("result") != values.end()) {
            int result = std::any_cast<int>(values.at("result"));
            std::cout << "Loop completed with result: " << result << "\n";
          }
        }
      );
    },
    [&counter](const std::unordered_map<std::string, std::any>& inputs) -> int {
      int result = (counter < 5) ? 0 : 1;
      std::cout << "  Condition check: counter=" << counter << ", result=" << result << " (should continue if 0)\n";
      return result; 
    },
    [](wf::GraphBuilder& gb, const std::unordered_map<std::string, std::any>& inputs) {
      // Build exit subgraph
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
        {{"exit_print", "done"}},
        [](const std::unordered_map<std::string, std::any>&) {
          std::cout << "Exit sink callback executed\n";
        }
      );
    },
    {"result"}  // output_keys
  );
#else
    
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
        {{"loop_iteration", "result"}},
        [](const std::unordered_map<std::string, std::any>& values) {
          // Custom callback to process the result
          if (values.find("result") != values.end()) {
            int result = std::any_cast<int>(values.at("result"));
            std::cout << "Loop completed with result: " << result << "\n";
          }
        }
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
        {{"exit_print", "done"}},
        [](const std::unordered_map<std::string, std::any>&) {
          std::cout << "Exit sink callback executed\n";
        }
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

#endif
  std::cout << "=== Running loop_only example ===\n";
  builder.run(executor);
  builder.dump(std::cout);
  std::cout << "=== Done ===\n";
  return 0;
}


