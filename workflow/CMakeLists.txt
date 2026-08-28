cmake_minimum_required(VERSION 3.18)

if(DEFINED Taskflow_VERSION)
  set(WORKFLOW_VERSION "${Taskflow_VERSION}")
else()
  set(WORKFLOW_VERSION "4.1.0")
endif()
project(workflow VERSION ${WORKFLOW_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
add_library(workflow::workflow ALIAS workflow)
target_compile_features(workflow PUBLIC cxx_std_20)
target_include_directories(workflow PUBLIC
  $<BUILD_INTERFACE:${WORKFLOW_INCLUDE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Ensure C++20 for workflow library (required for template lambdas)
set_property(TARGET workflow PROPERTY CXX_STANDARD 20)
set_property(TARGET workflow PROPERTY CXX_STANDARD_REQUIRED ON)

# For MSVC, explicitly set /std:c++20 flag
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(workflow PRIVATE /std:c++20)
endif()

if(BUILD_TESTING)
  enable_testing()
  add_executable(test_workflow_runtime_slots tests/test_runtime_slots.cpp)
  target_link_libraries(test_workflow_runtime_slots PRIVATE workflow)
  add_test(NAME workflow_runtime_slots COMMAND test_workflow_runtime_slots)
  set_tests_properties(workflow_runtime_slots PROPERTIES LABELS "workflow-control-flow")

  add_executable(test_workflow_modules tests/test_modules.cpp)
  target_link_libraries(test_workflow_modules PRIVATE workflow)
  add_test(NAME workflow_modules COMMAND test_workflow_modules)
  set_tests_properties(workflow_modules PROPERTIES LABELS "workflow-module")

  add_executable(test_workflow_control_flow tests/test_control_flow.cpp)
  target_link_libraries(test_workflow_control_flow PRIVATE workflow)
  add_test(NAME workflow_control_flow COMMAND test_workflow_control_flow)
  set_tests_properties(workflow_control_flow PROPERTIES LABELS "workflow-control-flow")

  add_test(
    NAME workflow_package_consumer
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DINSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package-prefix
      -DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/package-consumer
      -DCONSUMER_BUILD=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-build
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_package.cmake
  )
  set_tests_properties(workflow_package_consumer PROPERTIES LABELS "workflow-package")
endif()

# Link against Taskflow
if(TARGET Taskflow::Taskflow)
  target_link_libraries(workflow PUBLIC Taskflow::Taskflow)
elseif(TARGET Taskflow)
  target_link_libraries(workflow PUBLIC Taskflow)
else()
  find_package(Taskflow CONFIG REQUIRED)
  target_link_libraries(workflow PUBLIC Taskflow::Taskflow)
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

add_executable(control_flow_v4 examples/control_flow_v4.cpp)
set_property(TARGET control_flow_v4 PROPERTY CXX_STANDARD 20)
set_property(TARGET control_flow_v4 PROPERTY CXX_STANDARD_REQUIRED ON)
target_link_libraries(control_flow_v4 PRIVATE workflow)
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

# Installable CMake package
install(TARGETS workflow EXPORT workflowTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/workflow DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT workflowTargets
  FILE workflowTargets.cmake
  NAMESPACE workflow::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workflow
)
export(EXPORT workflowTargets
  FILE ${CMAKE_CURRENT_BINARY_DIR}/workflowTargets.cmake
  NAMESPACE workflow::
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/workflowConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/workflowConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workflow
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/workflowConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/workflowConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/workflowConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workflow
)
