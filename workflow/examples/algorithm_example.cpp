// Example demonstrating Taskflow algorithm nodes using declarative API
// This example shows:
//   1. create_for_each - parallel iteration over containers
//   2. create_for_each_index - parallel iteration over index ranges
//   3. create_reduce - parallel reduction
//   4. create_transform - parallel transformation
//
// Graph structure:
//   Input -> for_each (print elements) -> transform (square) -> reduce (sum) -> sink
//   Input -> for_each_index (print indices)

#include <workflow/nodeflow.hpp>
#include <taskflow/taskflow.hpp>
#include <iostream>
#include <vector>
#include <numeric>

int main() {
  namespace wf = workflow;
  
  tf::Executor executor;
  wf::GraphBuilder builder("algorithm_workflow");
  
  std::cout << "=== Taskflow Algorithm Nodes Example ===\n\n";
  
  // ==========================================================================
  // Create input data source
  // ==========================================================================
  std::vector<int> numbers;
  for (int i = 1; i <= 10; ++i) {
    numbers.push_back(i);
  }
  
  std::cout << "Input vector: [";
  for (size_t i = 0; i < numbers.size(); ++i) {
    std::cout << numbers[i];
    if (i < numbers.size() - 1) std::cout << ", ";
  }
  std::cout << "]\n\n";
  
  // Create source node with the vector
  auto [input_node, input_task] = builder.create_any_source("Input", 
    {{"data", std::any{numbers}}}
  );
  
  // ==========================================================================
  // Example 1: create_for_each - Parallel iteration over container
  // ==========================================================================
  std::cout << "1. Using create_for_each to print each element:\n";
  
  // Example 1a: create_for_each without shared parameters
  auto [for_each_node, for_each_task] = builder.create_for_each<std::vector<int>>(
    "PrintElements",
    {{"Input", "data"}},  // Input: container from Input node's "data" output (first element is container)
    std::function<void(int, std::unordered_map<std::string, std::any>&)>(
      [](int value, std::unordered_map<std::string, std::any>& shared_params) {
        // Even without using shared_params, it's required in the function signature
        (void)shared_params;  // Suppress unused parameter warning
        std::cout << "  Processing element: " << value << "\n";
      }
    ),
    {}  // No outputs for for_each
  );
  
  // ==========================================================================
  // Example 2: create_transform - Transform elements (square each number)
  // ==========================================================================
  std::cout << "\n2. Using create_transform to square each element:\n";
  
  auto [transform_node, transform_task] = builder.create_transform<std::vector<int>, std::vector<int>>(
    "SquareElements",
    {{"Input", "data"}},  // Input container
    std::function<int(int)>([](int val) -> int {
      return val * val;
    }),
    {"squared"}  // Output key
  );
  
  // Create a node to display the squared results
  auto [display_node, display_task] = builder.create_typed_node<std::vector<int>>(
    "DisplaySquared",
    {{"SquareElements", "squared"}},
    [](const std::tuple<std::vector<int>>& in) {
      const auto& squared = std::get<0>(in);
      std::cout << "  Squared vector: [";
      for (size_t i = 0; i < squared.size(); ++i) {
        std::cout << squared[i];
        if (i < squared.size() - 1) std::cout << ", ";
      }
      std::cout << "]\n";
      return std::make_tuple(squared);
    },
    {"squared"}
  );
  
  // ==========================================================================
  // Example 3: create_reduce - Reduce to sum
  // ==========================================================================
  std::cout << "\n3. Using create_reduce to compute sum:\n";
  
  // Initialize accumulator (must remain alive during execution)
  int sum_result = 0;
  
  // Create a reduce node using the new API
  auto [reduce_node, reduce_task] = builder.create_reduce<int, std::vector<int>>(
    "SumElements",
    {{"SquareElements", "squared"}},  // Input: container (first element)
    sum_result,  // Initial value (passed by reference)
    std::function<int(int, int, std::unordered_map<std::string, std::any>&)>(
      [](int acc, int element, std::unordered_map<std::string, std::any>& shared_params) -> int {
        return acc + element;  // Simple addition reduction
      }
    ),
    {"sum"}  // Output key
  );
  
  // Create a node to display the result
  auto [display_sum_node, display_sum_task] = builder.create_typed_node<int>(
    "DisplaySum",
    {{"SumElements", "sum"}},
    [](const std::tuple<int>& in) {
      int sum = std::get<0>(in);
      std::cout << "  Sum of squared elements: " << sum << "\n";
      return std::make_tuple(sum);
    },
    {"sum"}
  );
  
  // ==========================================================================
  // Example 4: create_for_each_index - Parallel iteration over index range
  // ==========================================================================
  std::cout << "\n4. Using create_for_each_index to print indices:\n";
  
  // Create shared parameter source (optional, for demonstration)
  auto [shared_param_node, shared_param_task] = builder.create_any_source("SharedParams",
    {{"multiplier", std::any{2}}}
  );
  
  // Use create_for_each_index with index range as function parameters
  auto [for_each_index_node, for_each_index_task] = builder.create_for_each_index<int>(
    "PrintIndices",
    {{"SharedParams", "multiplier"}},  // Shared parameters (optional)
    0,   // first: beginning index (inclusive)
    20,  // last: ending index (exclusive)
    2,   // step: step size
    std::function<void(int, std::unordered_map<std::string, std::any>&)>(
      [](int index, std::unordered_map<std::string, std::any>& shared_params) {
        int multiplier = std::any_cast<int>(shared_params.at("multiplier"));
        std::cout << "    Index: " << index << ", multiplied: " << (index * multiplier) << "\n";
      }
    ),
    {}  // No outputs
  );
  
  // ==========================================================================
  // Create sink to collect final results
  // ==========================================================================
  auto [sink, sink_task] = builder.create_any_sink("FinalSink",
    {{"DisplaySum", "sum"}},  // Updated to use DisplaySum node
    [](const std::unordered_map<std::string, std::any>& values) {
      if (values.find("sum") != values.end()) {
        int final_sum = std::any_cast<int>(values.at("sum"));
        std::cout << "\n=== Final Result ===\n";
        std::cout << "Sum of squares (1^2 + 2^2 + ... + 10^2) = " << final_sum << "\n";
        std::cout << "Expected: " << (10 * 11 * 21) / 6 << "\n";  // n(n+1)(2n+1)/6
      }
    }
  );
  
  // ==========================================================================
  // Run the workflow
  // ==========================================================================
  std::cout << "\n=== Running workflow ===\n";
  builder.run(executor);
  
  std::cout << "\n=== Workflow graph ===\n";
  builder.dump(std::cout);
  
  return 0;
}

