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
  
  auto [for_each_node, for_each_task] = builder.create_for_each<std::vector<int>>(
    "PrintElements",
    {{"Input", "data"}},  // Input: container from Input node's "data" output (first element is container)
    std::function<void(int, std::unordered_map<std::string, std::any>&)>(
      [](int value, std::unordered_map<std::string, std::any>& shared_params) {
        std::cout << "  Processing element: " << value << "\n";
        // shared_params can be used to modify shared state across iterations
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
  
  // Create a reduce node (this will be implemented)
  // For now, we'll use a regular node as a placeholder
  int sum_result = 0;
  auto [reduce_node, reduce_task] = builder.create_typed_node<std::vector<int>>(
    "SumElements",
    {{"SquareElements", "squared"}},
    [&sum_result](const std::tuple<std::vector<int>>& in) {
      const auto& vec = std::get<0>(in);
      sum_result = std::accumulate(vec.begin(), vec.end(), 0);
      std::cout << "  Sum of squared elements: " << sum_result << "\n";
      return std::make_tuple(sum_result);
    },
    {"sum"}
  );
  
  // ==========================================================================
  // Example 4: create_for_each_index - Parallel iteration over index range
  // ==========================================================================
  std::cout << "\n4. Using create_for_each_index to print indices:\n";
  
  // Create source with index range parameters
  auto [index_input, index_input_task] = builder.create_typed_source("IndexInput",
    std::make_tuple(0, 20, 2),  // first=0, last=20, step=2
    std::vector<std::string>{"first", "last", "step"}
  );
  
  // Note: create_for_each_index will be implemented later
  // For now, we'll use a regular node as placeholder
  auto [for_each_index_node, for_each_index_task] = builder.create_typed_node<int, int, int>(
    "PrintIndices",
    {{"IndexInput", "first"}, {"IndexInput", "last"}, {"IndexInput", "step"}},
    [](const std::tuple<int, int, int>& in) {
      int first = std::get<0>(in);
      int last = std::get<1>(in);
      int step = std::get<2>(in);
      std::cout << "  Indices from " << first << " to " << last << " (step " << step << "):\n";
      for (int i = first; i < last; i += step) {
        std::cout << "    Index: " << i << "\n";
      }
      return std::make_tuple(0);
    },
    {"done"}
  );
  
  // ==========================================================================
  // Create sink to collect final results
  // ==========================================================================
  auto [sink, sink_task] = builder.create_any_sink("FinalSink",
    {{"SumElements", "sum"}},
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

