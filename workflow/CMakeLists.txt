cmake_minimum_required(VERSION 3.18)
project(workflow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
set(WORKFLOW_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Workflow library with source files
file(GLOB_RECURSE WORKFLOW_SOURCES "src/*.cpp")
add_library(workflow STATIC ${WORKFLOW_SOURCES})
target_include_directories(workflow PUBLIC ${WORKFLOW_INCLUDE_DIR})

# Link against Taskflow
get_filename_component(TASKFLOW_PARENT_DIR ${CMAKE_SOURCE_DIR} DIRECTORY)
target_include_directories(workflow PUBLIC ${TASKFLOW_PARENT_DIR})
if(TARGET Taskflow::Taskflow)
  target_link_libraries(workflow PUBLIC Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(workflow PUBLIC Taskflow)
endif()

# Examples
add_executable(keyed_example examples/keyed_example.cpp)
target_link_libraries(keyed_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(keyed_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(keyed_example Taskflow)
endif()

add_executable(unified_example examples/unified_example.cpp)
target_link_libraries(unified_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(unified_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(unified_example Taskflow)
endif()

add_executable(declarative_example examples/declarative_example.cpp)
target_link_libraries(declarative_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(declarative_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(declarative_example Taskflow)
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

