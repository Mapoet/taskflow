cmake_minimum_required(VERSION 3.18)
project(workflow LANGUAGES CXX)

# Use C++20 for template lambda support (required by nodeflow_impl.hpp)
# Can be overridden by parent CMakeLists.txt via CMAKE_CXX_STANDARD
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    # If parent sets C++ standard, use it but warn if < 20
    if(CMAKE_CXX_STANDARD VERSION_LESS "20")
        message(WARNING "workflow uses C++20 features (template lambdas). "
                "Setting CMAKE_CXX_STANDARD to 20 for workflow targets.")
        set(CMAKE_CXX_STANDARD 20)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    else()
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    endif()
endif()

# Include directories
set(WORKFLOW_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Workflow library with source files
file(GLOB_RECURSE WORKFLOW_SOURCES "src/*.cpp")
add_library(workflow STATIC ${WORKFLOW_SOURCES})
target_include_directories(workflow PUBLIC ${WORKFLOW_INCLUDE_DIR})

# Ensure C++20 for workflow library (required for template lambdas)
set_property(TARGET workflow PROPERTY CXX_STANDARD 20)
set_property(TARGET workflow PROPERTY CXX_STANDARD_REQUIRED ON)

# For MSVC, explicitly set /std:c++20 flag
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(workflow PRIVATE /std:c++20)
endif()

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
set_property(TARGET keyed_example PROPERTY CXX_STANDARD 20)
set_property(TARGET keyed_example PROPERTY CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(keyed_example PRIVATE /std:c++20)
endif()
target_link_libraries(keyed_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(keyed_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(keyed_example Taskflow)
endif()

add_executable(unified_example examples/unified_example.cpp)
set_property(TARGET unified_example PROPERTY CXX_STANDARD 20)
set_property(TARGET unified_example PROPERTY CXX_STANDARD_REQUIRED ON)
# For MSVC, explicitly set /std:c++20 flag
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(unified_example PRIVATE /std:c++20)
endif()
target_link_libraries(unified_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(unified_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(unified_example Taskflow)
endif()

add_executable(declarative_example examples/declarative_example.cpp)
set_property(TARGET declarative_example PROPERTY CXX_STANDARD 20)
set_property(TARGET declarative_example PROPERTY CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(declarative_example PRIVATE /std:c++20)
endif()
target_link_libraries(declarative_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(declarative_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(declarative_example Taskflow)
endif()

add_executable(advanced_control_flow examples/advanced_control_flow.cpp)
set_property(TARGET advanced_control_flow PROPERTY CXX_STANDARD 20)
set_property(TARGET advanced_control_flow PROPERTY CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(advanced_control_flow PRIVATE /std:c++20)
endif()
target_link_libraries(advanced_control_flow workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(advanced_control_flow Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(advanced_control_flow Taskflow)
endif()

# Loop-only example
add_executable(loop_only examples/loop_only.cpp)
set_property(TARGET loop_only PROPERTY CXX_STANDARD 20)
set_property(TARGET loop_only PROPERTY CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(loop_only PRIVATE /std:c++20)
endif()
target_link_libraries(loop_only workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(loop_only Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(loop_only Taskflow)
endif()

# Algorithm nodes example
add_executable(algorithm_example examples/algorithm_example.cpp)
set_property(TARGET algorithm_example PROPERTY CXX_STANDARD 20)
set_property(TARGET algorithm_example PROPERTY CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(algorithm_example PRIVATE /std:c++20)
endif()
target_link_libraries(algorithm_example workflow)
if(TARGET Taskflow::Taskflow)
  target_link_libraries(algorithm_example Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(algorithm_example Taskflow)
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

