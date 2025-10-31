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

  // ==========================================================================
  // Example 1: Condition Node (if-else branching)
  // ==========================================================================
  std::cout << "1. Condition Node (if-else):\n";

  auto [A, tA] = builder.create_typed_source("A",
    std::make_tuple(42),
    {"value"}
  );

  // Condition node: returns 0 for even, 1 for odd
  auto [B, tB] = builder.create_condition_node("B",
    []() -> int {
      // In real usage, this would access A's output
      // For demo, return 0 (even branch)
      return 0;
    }
  );

  // Branch nodes (created manually for now)
  auto C_task = builder.taskflow().emplace([]() {
    std::cout << "  -> Even branch (C)\n";
  }).name("C");

  auto D_task = builder.taskflow().emplace([]() {
    std::cout << "  -> Odd branch (D)\n";
  }).name("D");

  // Set up condition: B precedes both branches
  tB.precede(C_task, D_task);

  std::cout << "  A -> B (condition) -> C or D\n";

  // ==========================================================================
  // Example 2: Multi-Condition Node (parallel branching)
  // ==========================================================================
  std::cout << "\n2. Multi-Condition Node (parallel branches):\n";

  auto [E, tE] = builder.create_typed_source("E",
    std::make_tuple(100),
    {"data"}
  );

  // Multi-condition: returns {0, 2} to execute branches 0 and 2 in parallel
  auto [F, tF] = builder.create_multi_condition_node("F",
    []() -> tf::SmallVector<int> {
      return {0, 2};  // Execute first and third branches
    }
  );

  // Multiple branch nodes
  auto G_task = builder.taskflow().emplace([]() {
    std::cout << "  -> Branch G (executed)\n";
  }).name("G");

  auto H_task = builder.taskflow().emplace([]() {
    std::cout << "  -> Branch H (not executed)\n";
  }).name("H");

  auto I_task = builder.taskflow().emplace([]() {
    std::cout << "  -> Branch I (executed)\n";
  }).name("I");

  // Multi-condition precedes all branches
  tF.precede(G_task, H_task, I_task);

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

  // ==========================================================================
  // Example 4: Loop Node
  // ==========================================================================
  std::cout << "\n4. Loop Node:\n";

  int counter = 0;

  // Loop body
  auto loop_body = [&counter]() {
    std::cout << "  Loop iteration: counter = " << counter << "\n";
    counter++;
  };

  // Loop condition: continue while counter < 5
  auto loop_condition = [&counter]() -> int {
    return (counter < 5) ? 0 : 1;  // 0 to continue, 1 to exit
  };

  auto [Loop, tLoop] = builder.create_loop_node("Loop",
    loop_body,
    loop_condition
  );

  // Get the condition task from the loop structure
  // The loop creates: body_task -> cond_task
  // We need to manually add: cond_task -> body_task (for loop back)
  // And: cond_task -> exit_task (for exit)
  
  // Create exit task
  auto exit_task = builder.taskflow().emplace([]() {
    std::cout << "  Loop exited\n";
  }).name("Loop_exit");

  // Find the condition task (it's named "Loop_condition")
  // For now, we'll use a different approach: manually create the loop structure
  
  std::cout << "  Loop structure: body -> condition -> (body if 0, exit if non-zero)\n";

  // ==========================================================================
  // Manual loop construction (more control)
  // ==========================================================================
  std::cout << "\n4b. Manual Loop Construction:\n";
  
  int manual_counter = 0;
  
  auto manual_body = builder.taskflow().emplace([&manual_counter]() {
    std::cout << "  Manual loop iteration: counter = " << manual_counter << "\n";
    manual_counter++;
  }).name("ManualLoop_body");
  
  auto manual_cond = builder.taskflow().emplace([&manual_counter]() -> int {
    return (manual_counter < 3) ? 0 : 1;
  }).name("ManualLoop_condition");
  
  auto manual_exit = builder.taskflow().emplace([]() {
    std::cout << "  Manual loop exited\n";
  }).name("ManualLoop_exit");
  
  // Loop structure: body -> condition -> (body if 0, exit if 1)
  manual_body.precede(manual_cond);
  manual_cond.precede(manual_body, manual_exit);  // Index 0 = body (loop), index 1 = exit

  // ==========================================================================
  // Execute
  // ==========================================================================
  
  std::cout << "\n=== Executing workflow ===\n\n";
  
  builder.run(executor);
  
  std::cout << "\n=== Workflow completed ===\n";
  builder.dump(std::cout);
  
  return 0;
}

