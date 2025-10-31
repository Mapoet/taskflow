cmake_minimum_required(VERSION 3.18)
project(workflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
set(WORKFLOW_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Workflow is header-only interface library
add_library(workflow INTERFACE)
target_include_directories(workflow INTERFACE ${WORKFLOW_INCLUDE_DIR})

# If you add .cpp files later, change to:
# add_library(workflow STATIC src/nodeflow.cpp)
# target_include_directories(workflow PUBLIC ${WORKFLOW_INCLUDE_DIR})

# Link against Taskflow
# When built as subdirectory, Taskflow target should already be available
# When built standalone, we need to find Taskflow headers
if(TARGET Taskflow::Taskflow)
  # Taskflow is already configured (built as subdirectory)
  target_link_libraries(workflow INTERFACE Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(workflow INTERFACE Taskflow)
else()
  # Standalone build: assume Taskflow headers in parent directory
  get_filename_component(TASKFLOW_PARENT_DIR ${CMAKE_SOURCE_DIR} DIRECTORY)
  target_include_directories(workflow INTERFACE ${TASKFLOW_PARENT_DIR})
endif()

# Examples
add_executable(keyed_example examples/keyed_example.cpp)
target_link_libraries(keyed_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(keyed_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(keyed_example Taskflow)
endif()

# Install rules (optional)
# install(TARGETS workflow EXPORT workflowTargets
#   ARCHIVE DESTINATION lib
#   LIBRARY DESTINATION lib
#   RUNTIME DESTINATION bin)
# install(FILES include/workflow/nodeflow.hpp
#   DESTINATION include/workflow)
# install(EXPORT workflowTargets
#   FILE workflowTargets.cmake
#   NAMESPACE workflow::
#   DESTINATION lib/cmake/workflow)

