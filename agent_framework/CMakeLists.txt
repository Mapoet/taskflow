cmake_minimum_required(VERSION 3.20)
project(AgentFramework VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -march=native -DNDEBUG)
else()
    add_compile_options(-g -O0 -Wall -Wextra)
endif()

# 检查是否从根目录调用（已构建 Taskflow 和 workflow）
# 如果从根目录调用，Taskflow 和 workflow 已经构建，直接使用
# 如果独立调用，需要构建 Taskflow

# 检查 Taskflow 是否已构建（从根目录调用时）
if(TARGET Taskflow)
    # 从根目录调用，Taskflow 和 workflow 已构建
    message(STATUS "Agent Framework: Using Taskflow from parent CMakeLists.txt")
elseif(TARGET taskflow)
    # 备选名称
    message(STATUS "Agent Framework: Using taskflow from parent CMakeLists.txt")
else()
    # 独立调用，需要构建 Taskflow
    message(STATUS "Agent Framework: Building Taskflow as dependency")
    set(TASKFLOW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../")
    add_subdirectory(${TASKFLOW_DIR} ${CMAKE_BINARY_DIR}/taskflow_subdir EXCLUDE_FROM_ALL)
    
    # 启用 workflow 库
    set(TF_BUILD_WORKFLOW ON)
endif()

# 第三方库 submodules（使用根目录的 3rd-party）
# 注意：第三方库位于项目根目录的 3rd-party/ 目录中
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party)

# nlohmann/json (Header-Only, single header file)
# 检查多个可能的位置
set(NLOHMANN_JSON_INCLUDE "")

# 优先检查 nlohmann_json (submodule)
if(EXISTS ${THIRD_PARTY_DIR}/nlohmann_json/include/nlohmann/json.hpp)
    set(NLOHMANN_JSON_INCLUDE ${THIRD_PARTY_DIR}/nlohmann_json/include)
    message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE}")
elseif(EXISTS ${THIRD_PARTY_DIR}/nlohmann_json/single_include/nlohmann/json.hpp)
    set(NLOHMANN_JSON_INCLUDE ${THIRD_PARTY_DIR}/nlohmann_json/single_include)
    message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE}")
# 检查 nlohmann (可能已存在的目录)
elseif(EXISTS ${THIRD_PARTY_DIR}/nlohmann/include/nlohmann/json.hpp)
    set(NLOHMANN_JSON_INCLUDE ${THIRD_PARTY_DIR}/nlohmann/include)
    message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE}")
elseif(EXISTS ${THIRD_PARTY_DIR}/nlohmann/single_include/nlohmann/json.hpp)
    set(NLOHMANN_JSON_INCLUDE ${THIRD_PARTY_DIR}/nlohmann/single_include)
    message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE}")
# 检查单文件 json.hpp（在 nlohmann/ 目录下直接）
elseif(EXISTS ${THIRD_PARTY_DIR}/nlohmann/json.hpp)
    # 文件在 nlohmann/json.hpp，但代码使用 #include <nlohmann/json.hpp>
    # 所以需要包含 nlohmann 的父目录
    set(NLOHMANN_JSON_INCLUDE ${THIRD_PARTY_DIR})
    message(STATUS "Found nlohmann/json at: ${NLOHMANN_JSON_INCLUDE} (single file in nlohmann/)")
else()
    message(STATUS "nlohmann/json not found (optional). Run: git submodule update --init --recursive")
    set(NLOHMANN_JSON_INCLUDE "")
endif()

# httplib (Header-Only)
set(HTTPLIB_DIR ${THIRD_PARTY_DIR}/httplib)
if(EXISTS ${HTTPLIB_DIR}/httplib.hpp)
    message(STATUS "Found httplib at: ${HTTPLIB_DIR}")
    # httplib is header-only, just add include path
elseif(EXISTS ${HTTPLIB_DIR}/httplib.h)
    message(STATUS "Found httplib at: ${HTTPLIB_DIR}")
    # httplib is header-only, just add include path
else()
    message(STATUS "httplib not found (optional). Run: git submodule update --init --recursive")
endif()

# websocketpp (Header-Only)
set(WEBSOCKETPP_DIR ${THIRD_PARTY_DIR}/websocketpp)
if(EXISTS ${WEBSOCKETPP_DIR})
    # websocketpp is header-only, just add include path
else()
    message(WARNING "websocketpp not found in ${WEBSOCKETPP_DIR}. Run: git submodule update --init --recursive")
endif()

# Faiss (可选，需要编译)
option(AGENT_BUILD_FAISS "Build Faiss vector database support" OFF)
if(AGENT_BUILD_FAISS)
    set(FAISS_DIR ${THIRD_PARTY_DIR}/faiss)
    if(EXISTS ${FAISS_DIR}/CMakeLists.txt)
        add_subdirectory(${FAISS_DIR} EXCLUDE_FROM_ALL)
    else()
        message(WARNING "Faiss not found in ${FAISS_DIR}. Run: git submodule update --init --recursive")
    endif()
endif()

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../taskflow
    ${CMAKE_CURRENT_SOURCE_DIR}/../workflow/include
)

# 第三方库包含目录（仅在存在时添加）
if(NLOHMANN_JSON_INCLUDE)
    include_directories(${NLOHMANN_JSON_INCLUDE})
endif()

if(EXISTS ${HTTPLIB_DIR}/httplib.hpp OR EXISTS ${HTTPLIB_DIR}/httplib.h)
    include_directories(${HTTPLIB_DIR})
endif()

if(EXISTS ${WEBSOCKETPP_DIR})
    include_directories(${WEBSOCKETPP_DIR})
endif()

# 查找依赖（可选）
find_package(OpenCV QUIET)
find_package(CURL QUIET)

# 源文件
set(AGENT_SOURCES
    src/llm_client/llm_client.cpp
    src/llm_client/openai_adapter.cpp
    src/llm_client/anthropic_adapter.cpp
    src/llm_client/vllm_adapter.cpp
    src/toolbus/toolbus.cpp
    src/toolbus/local_tool.cpp
    src/toolbus/mcp_client.cpp
    src/memory/memory_store.cpp
    src/memory/event_logger.cpp
    src/vectorstore/vectorstore.cpp
    src/vectorstore/faiss_adapter.cpp
    src/vectorstore/encoder_manager.cpp
    src/graph_executor/graph_executor.cpp
    src/graph_executor/agent_templates.cpp
    src/graph_executor/workflow_builder.cpp
    src/ui/ui_manager.cpp
    src/ui/cli_handler.cpp
    src/ui/gui_handler.cpp
    src/ui/web_handler.cpp
    src/node/node_factory.cpp
    src/node/llm_node.cpp
    src/node/knowledge_base_node.cpp
    src/node/tool_call_node.cpp
    src/node/agent_loop_node.cpp
    src/node/ui_sink_node.cpp
)

# 头文件
set(AGENT_HEADERS
    include/agent/types.hpp
    include/agent/llm_client.hpp
    include/agent/prompt_renderer.hpp
    include/agent/toolbus.hpp
    include/agent/mcp_client.hpp
    include/agent/memory.hpp
    include/agent/vectorstore.hpp
    include/agent/encoder.hpp
    include/agent/graph_executor.hpp
    include/agent/ui_manager.hpp
    include/node/node_factory.hpp
    include/node/llm_node.hpp
    include/node/knowledge_base_node.hpp
    include/node/tool_call_node.hpp
    include/node/agent_loop_node.hpp
    include/node/ui_sink_node.hpp
    include/node/nodes.hpp
)

# 创建库
add_library(agent_framework STATIC ${AGENT_SOURCES} ${AGENT_HEADERS})

# 链接 Taskflow 和 workflow 库
# 检查库名称（可能是 Taskflow、Taskflow::Taskflow 或 taskflow）
if(TARGET Taskflow)
    target_link_libraries(agent_framework PRIVATE Taskflow)
elseif(TARGET Taskflow::Taskflow)
    target_link_libraries(agent_framework PRIVATE Taskflow::Taskflow)
elseif(TARGET taskflow)
    target_link_libraries(agent_framework PRIVATE taskflow)
else()
    message(FATAL_ERROR "Taskflow library not found. Please ensure Taskflow is built first.")
endif()

if(TARGET workflow)
    target_link_libraries(agent_framework PRIVATE workflow)
else()
    message(WARNING "Workflow library not found. Some features may not work.")
endif()

if(OpenCV_FOUND)
    target_link_libraries(agent_framework PRIVATE ${OpenCV_LIBS})
    target_compile_definitions(agent_framework PRIVATE HAVE_OPENCV)
endif()

if(CURL_FOUND)
    target_link_libraries(agent_framework PRIVATE CURL::libcurl)
    target_compile_definitions(agent_framework PRIVATE HAVE_CURL)
endif()

# 示例程序
add_executable(simple_agent examples/simple_agent.cpp)
target_link_libraries(simple_agent PRIVATE agent_framework)

add_executable(multimodal_agent examples/multimodal_agent.cpp)
target_link_libraries(multimodal_agent PRIVATE agent_framework)

add_executable(tool_integration examples/tool_integration.cpp)
target_link_libraries(tool_integration PRIVATE agent_framework)

add_executable(workflow_custom examples/workflow_custom.cpp)
target_link_libraries(workflow_custom PRIVATE agent_framework)

# 测试（需要 Google Test 或 Catch2）
if(BUILD_TESTING)
    enable_testing()
    # TODO: 添加测试
endif()

# 安装规则
install(TARGETS agent_framework
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/agent
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

