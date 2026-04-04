cmake_minimum_required(VERSION 3.20)
project(AgentFramework VERSION 1.0.0 LANGUAGES CXX)

# 检查父项目是否已设置 C++ 标准
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
else()
    # 如果父项目已设置，确保至少是 C++17
    if(CMAKE_CXX_STANDARD LESS 17)
        message(WARNING "C++ standard is ${CMAKE_CXX_STANDARD}, but at least C++17 is required. Setting to C++17.")
        set(CMAKE_CXX_STANDARD 17)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    endif()
endif()

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

# 示例可执行文件（含 WP1.6 cli_agent_demo、WP1.8 cli_agent_skills_demo）；Taskflow 根工程下默认与 TF_BUILD_EXAMPLES 一致
if(DEFINED TF_BUILD_EXAMPLES AND NOT TF_BUILD_EXAMPLES)
    option(AGENT_BUILD_EXAMPLES
           "Build Agent Framework examples (cli_agent_demo, simple_agent, multimodal_agent, ...)" OFF)
else()
    option(AGENT_BUILD_EXAMPLES
           "Build Agent Framework examples (cli_agent_demo, simple_agent, multimodal_agent, ...)" ON)
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
find_package(OpenSSL QUIET)
find_package(Threads QUIET)

# 线程支持（用于 std::thread，SSEConnection 等）
if(CMAKE_THREAD_LIBS_INIT OR Threads_FOUND)
    set(CMAKE_USE_PTHREADS_INIT ON)
endif()

# 源文件（条件添加，仅当文件存在时）
set(AGENT_SOURCES "")

# LLM Client 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/llm_client.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/llm_client.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/model_adapter.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/model_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/openai_adapter.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/openai_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/anthropic_adapter.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/anthropic_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/gemini_adapter.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/gemini_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_client/vllm_adapter.cpp)
    list(APPEND AGENT_SOURCES src/llm_client/vllm_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_renderer/prompt_renderer.cpp)
    list(APPEND AGENT_SOURCES src/prompt_renderer/prompt_renderer.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_renderer/prompt_template.cpp)
    list(APPEND AGENT_SOURCES src/prompt_renderer/prompt_template.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_renderer/tool_formatter.cpp)
    list(APPEND AGENT_SOURCES src/prompt_renderer/tool_formatter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/prompt_renderer/history_formatter.cpp)
    list(APPEND AGENT_SOURCES src/prompt_renderer/history_formatter.cpp)
endif()

# WP1.8 Skills
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/skills/skill_frontmatter_parse.cpp)
    list(APPEND AGENT_SOURCES src/skills/skill_frontmatter_parse.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/skills/skill_registry.cpp)
    list(APPEND AGENT_SOURCES src/skills/skill_registry.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/skills/skill_loader.cpp)
    list(APPEND AGENT_SOURCES src/skills/skill_loader.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/skills/skill_services.cpp)
    list(APPEND AGENT_SOURCES src/skills/skill_services.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/skills/skill_script_tool.cpp)
    list(APPEND AGENT_SOURCES src/skills/skill_script_tool.cpp)
endif()

# ToolBus 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/toolbus.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/toolbus.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/local_tool.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/local_tool.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/fs_sandbox.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/fs_sandbox.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/fs_tools.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/fs_tools.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_http.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_http.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_search_ddg.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_search_ddg.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_fetch.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_fetch.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_rss.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_rss.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_archive.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_archive.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/web_tools.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/web_tools.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/news_sources_catalog.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/news_sources_catalog.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/news_sources_tool.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/news_sources_tool.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/schema_validate.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/schema_validate.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/toolbus/mcptool.cpp)
    list(APPEND AGENT_SOURCES src/toolbus/mcptool.cpp)
endif()

# MCP 客户端（WP1.3）
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp_client/mcp_client.cpp)
    list(APPEND AGENT_SOURCES src/mcp_client/mcp_client.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp_client/stdio_transport.cpp)
    list(APPEND AGENT_SOURCES src/mcp_client/stdio_transport.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp_client/http_transport.cpp)
    list(APPEND AGENT_SOURCES src/mcp_client/http_transport.cpp)
endif()

# Memory 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/memory/memory_store.cpp)
    list(APPEND AGENT_SOURCES src/memory/memory_store.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/memory/event_logger.cpp)
    list(APPEND AGENT_SOURCES src/memory/event_logger.cpp)
endif()

# VectorStore 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/vectorstore/vectorstore.cpp)
    list(APPEND AGENT_SOURCES src/vectorstore/vectorstore.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/vectorstore/faiss_adapter.cpp)
    list(APPEND AGENT_SOURCES src/vectorstore/faiss_adapter.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/vectorstore/encoder_manager.cpp)
    list(APPEND AGENT_SOURCES src/vectorstore/encoder_manager.cpp)
endif()

# GraphExecutor 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_executor/graph_executor.cpp)
    list(APPEND AGENT_SOURCES src/graph_executor/graph_executor.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_executor/cli_agent_graph.cpp)
    list(APPEND AGENT_SOURCES src/graph_executor/cli_agent_graph.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_executor/agent_templates.cpp)
    list(APPEND AGENT_SOURCES src/graph_executor/agent_templates.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/graph_executor/workflow_builder.cpp)
    list(APPEND AGENT_SOURCES src/graph_executor/workflow_builder.cpp)
endif()

# UI 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ui_manager.cpp)
    list(APPEND AGENT_SOURCES src/ui/ui_manager.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/cli_handler.cpp)
    list(APPEND AGENT_SOURCES src/ui/cli_handler.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/gui_handler.cpp)
    list(APPEND AGENT_SOURCES src/ui/gui_handler.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/web_handler.cpp)
    list(APPEND AGENT_SOURCES src/ui/web_handler.cpp)
endif()

# Node 模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/node_factory.cpp)
    list(APPEND AGENT_SOURCES src/node/node_factory.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/llm_node.cpp)
    list(APPEND AGENT_SOURCES src/node/llm_node.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/knowledge_base_node.cpp)
    list(APPEND AGENT_SOURCES src/node/knowledge_base_node.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/tool_call_node.cpp)
    list(APPEND AGENT_SOURCES src/node/tool_call_node.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/agent_loop_node.cpp)
    list(APPEND AGENT_SOURCES src/node/agent_loop_node.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/node/ui_sink_node.cpp)
    list(APPEND AGENT_SOURCES src/node/ui_sink_node.cpp)
endif()

# A2A (Agent2Agent) 协议模块
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_transport/agent_transport.cpp)
    list(APPEND AGENT_SOURCES src/agent_transport/agent_transport.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_transport/sse_connection.cpp)
    list(APPEND AGENT_SOURCES src/agent_transport/sse_connection.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_client/agent_client.cpp)
    list(APPEND AGENT_SOURCES src/agent_client/agent_client.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_client/httplib_http_client.cpp)
    list(APPEND AGENT_SOURCES src/agent_client/httplib_http_client.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_server/agent_server.cpp)
    list(APPEND AGENT_SOURCES src/agent_server/agent_server.cpp)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/agent_types_serialization.cpp)
    list(APPEND AGENT_SOURCES src/agent_types_serialization.cpp)
endif()

# 头文件
set(AGENT_HEADERS
    include/agent/types.hpp
    include/agent/llm_client.hpp
    include/agent/prompt_renderer.hpp
    include/agent/schema_validate.hpp
    include/agent/mcp_protocol.hpp
    include/agent/toolbus.hpp
    include/agent/mcp_client.hpp
    include/agent/memory.hpp
    include/agent/vectorstore.hpp
    include/agent/encoder.hpp
    include/agent/graph_executor.hpp
    include/agent/ui_manager.hpp
    include/agent/agent_transport.hpp
    include/agent/sse_connection.hpp
    include/agent/agent_client.hpp
    include/agent/httplib_http_client.hpp
    include/agent/agent_server.hpp
    include/node/node_factory.hpp
    include/node/llm_node.hpp
    include/node/knowledge_base_node.hpp
    include/node/tool_call_node.hpp
    include/node/agent_loop_node.hpp
    include/node/ui_sink_node.hpp
    include/node/nodes.hpp
    include/agent/skill_types.hpp
    include/agent/skill_registry.hpp
    include/agent/skill_loader.hpp
    include/agent/skill_services.hpp
    include/agent/skill_script_tool.hpp
    include/agent/internal/skill_frontmatter_parse.hpp
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

if(OpenSSL_FOUND)
    target_link_libraries(agent_framework PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(agent_framework PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    message(STATUS "Agent Framework: OpenSSL found, https enabled for cpp-httplib client")
else()
    message(STATUS "Agent Framework: OpenSSL not found; HttplibClient https disabled (http only)")
endif()

find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(agent_framework PRIVATE ZLIB::ZLIB)
    target_compile_definitions(agent_framework PRIVATE AGENT_HAVE_ZLIB=1)
    message(STATUS "Agent Framework: zlib found; web_fetch_archive supports deflate")
endif()

# 线程库（用于 std::thread）
if(CMAKE_USE_PTHREADS_INIT)
    target_link_libraries(agent_framework PRIVATE Threads::Threads)
endif()

# MSVC 特殊处理：确保 C++20 支持
if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
    target_compile_options(agent_framework PRIVATE /std:c++20)
endif()

# 示例程序：AGENT_BUILD_EXAMPLES=ON（默认 ON，或与根目录 TF_BUILD_EXAMPLES 一致）且源文件存在时注册目标
set(_agent_framework_example_targets "")
if(AGENT_BUILD_EXAMPLES)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/simple_agent.cpp)
        add_executable(simple_agent examples/simple_agent.cpp)
        target_link_libraries(simple_agent PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(simple_agent PRIVATE Threads::Threads)
        endif()
        if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
            target_compile_options(simple_agent PRIVATE /std:c++20)
        endif()
        list(APPEND _agent_framework_example_targets simple_agent)
    endif()

    # WP1.6：CLI 示例；依赖 header-only CLI11（3rd-party/CLI11/CLI11.hpp）
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/cli_agent_demo.cpp)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/CLI11/CLI11.hpp)
            message(WARNING "cli_agent_demo: missing ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/CLI11/CLI11.hpp "
                            "(init CLI11 submodule or copy header)")
        else()
            add_executable(cli_agent_demo examples/cli_agent_demo.cpp)
            target_include_directories(cli_agent_demo PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/CLI11)
            target_link_libraries(cli_agent_demo PRIVATE agent_framework)
            if(CMAKE_USE_PTHREADS_INIT)
                target_link_libraries(cli_agent_demo PRIVATE Threads::Threads)
            endif()
            if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
                target_compile_options(cli_agent_demo PRIVATE /std:c++20)
            endif()
            list(APPEND _agent_framework_example_targets cli_agent_demo)
        endif()
    endif()

    # WP1.8：与 cli_agent_demo 相同链路，默认合并 ~/.cursor/skills 与 ~/.cursor/skills-cursor
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/cli_agent_skills_demo.cpp)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/CLI11/CLI11.hpp)
            message(WARNING "cli_agent_skills_demo: missing CLI11 (same as cli_agent_demo)")
        else()
            add_executable(cli_agent_skills_demo examples/cli_agent_skills_demo.cpp)
            target_include_directories(cli_agent_skills_demo PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../3rd-party/CLI11)
            target_link_libraries(cli_agent_skills_demo PRIVATE agent_framework)
            if(CMAKE_USE_PTHREADS_INIT)
                target_link_libraries(cli_agent_skills_demo PRIVATE Threads::Threads)
            endif()
            if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
                target_compile_options(cli_agent_skills_demo PRIVATE /std:c++20)
            endif()
            list(APPEND _agent_framework_example_targets cli_agent_skills_demo)
        endif()
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/multimodal_agent.cpp)
        add_executable(multimodal_agent examples/multimodal_agent.cpp)
        target_link_libraries(multimodal_agent PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(multimodal_agent PRIVATE Threads::Threads)
        endif()
        if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
            target_compile_options(multimodal_agent PRIVATE /std:c++20)
        endif()
        list(APPEND _agent_framework_example_targets multimodal_agent)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/tool_integration.cpp)
        add_executable(tool_integration examples/tool_integration.cpp)
        target_link_libraries(tool_integration PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(tool_integration PRIVATE Threads::Threads)
        endif()
        if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
            target_compile_options(tool_integration PRIVATE /std:c++20)
        endif()
        list(APPEND _agent_framework_example_targets tool_integration)
    endif()

    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/workflow_custom.cpp)
        add_executable(workflow_custom examples/workflow_custom.cpp)
        target_link_libraries(workflow_custom PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(workflow_custom PRIVATE Threads::Threads)
        endif()
        if(MSVC AND CMAKE_CXX_STANDARD GREATER_EQUAL 20)
            target_compile_options(workflow_custom PRIVATE /std:c++20)
        endif()
        list(APPEND _agent_framework_example_targets workflow_custom)
    endif()

    if(_agent_framework_example_targets)
        add_custom_target(agent_framework_examples DEPENDS ${_agent_framework_example_targets})
        string(REPLACE ";" " " _agent_ex_label "${_agent_framework_example_targets}")
        message(STATUS "Agent Framework: examples -> ${_agent_ex_label} (target: agent_framework_examples)")
    endif()
else()
    message(STATUS "Agent Framework: examples disabled (AGENT_BUILD_EXAMPLES=OFF); cli demos not built")
endif()

# 测试（WP1.1 LLM 轻量单测，无第三方框架）
if(BUILD_TESTING)
    enable_testing()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_llm_client_wp1.cpp)
        add_executable(test_llm_client_wp1 tests/test_llm_client_wp1.cpp)
        target_link_libraries(test_llm_client_wp1 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_llm_client_wp1 PRIVATE Threads::Threads)
        endif()
        if(OpenSSL_FOUND)
            target_link_libraries(test_llm_client_wp1 PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        add_test(NAME llm_client_wp1 COMMAND test_llm_client_wp1)
        set_tests_properties(llm_client_wp1 PROPERTIES
            ENVIRONMENT "AGENT_TEST_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/llm")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_toolbus_wp2.cpp)
        add_executable(test_toolbus_wp2 tests/test_toolbus_wp2.cpp)
        target_link_libraries(test_toolbus_wp2 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_toolbus_wp2 PRIVATE Threads::Threads)
        endif()
        add_test(NAME toolbus_wp2 COMMAND test_toolbus_wp2)
        add_test(NAME toolbus_wp2_allowlist COMMAND test_toolbus_wp2 --allowlist)
        # 避免继承外壳环境中的 AGENT_TOOL_ALLOWLIST，保证核心用例可注册任意工具名
        set_tests_properties(toolbus_wp2 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
        set_tests_properties(toolbus_wp2_allowlist PROPERTIES
            ENVIRONMENT "AGENT_TOOL_ALLOWLIST=add")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp_wp3.cpp)
        add_executable(test_mcp_wp3 tests/test_mcp_wp3.cpp)
        target_link_libraries(test_mcp_wp3 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_mcp_wp3 PRIVATE Threads::Threads)
        endif()
        if(OpenSSL_FOUND)
            target_link_libraries(test_mcp_wp3 PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        add_test(NAME mcp_wp3 COMMAND test_mcp_wp3)
        set_tests_properties(mcp_wp3 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cursor_mcp_import_wp3.cpp)
        add_executable(test_cursor_mcp_import_wp3 tests/test_cursor_mcp_import_wp3.cpp)
        target_link_libraries(test_cursor_mcp_import_wp3 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_cursor_mcp_import_wp3 PRIVATE Threads::Threads)
        endif()
        if(OpenSSL_FOUND)
            target_link_libraries(test_cursor_mcp_import_wp3 PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        add_test(NAME cursor_mcp_import_wp3 COMMAND test_cursor_mcp_import_wp3)
        set_tests_properties(cursor_mcp_import_wp3 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()

    # WP1.6: guard smoke (offline)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_loop_guard_wp16.cpp)
        add_executable(test_agent_loop_guard_wp16 tests/test_agent_loop_guard_wp16.cpp)
        target_link_libraries(test_agent_loop_guard_wp16 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_agent_loop_guard_wp16 PRIVATE Threads::Threads)
        endif()
        add_test(NAME agent_loop_guard_wp16 COMMAND test_agent_loop_guard_wp16)
        set_tests_properties(agent_loop_guard_wp16 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prompt_renderer_wp4.cpp)
        add_executable(test_prompt_renderer_wp4 tests/test_prompt_renderer_wp4.cpp)
        target_link_libraries(test_prompt_renderer_wp4 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_prompt_renderer_wp4 PRIVATE Threads::Threads)
        endif()
        add_test(NAME prompt_renderer_wp4 COMMAND test_prompt_renderer_wp4)
        set_tests_properties(prompt_renderer_wp4 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_build_cli_agent_graph.cpp)
        add_executable(test_build_cli_agent_graph tests/test_build_cli_agent_graph.cpp)
        target_link_libraries(test_build_cli_agent_graph PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_build_cli_agent_graph PRIVATE Threads::Threads)
        endif()
        add_test(NAME build_cli_agent_graph COMMAND test_build_cli_agent_graph)
        set_tests_properties(build_cli_agent_graph PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_loop_wp5.cpp)
        add_executable(test_agent_loop_wp5 tests/test_agent_loop_wp5.cpp)
        target_link_libraries(test_agent_loop_wp5 PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_agent_loop_wp5 PRIVATE Threads::Threads)
        endif()
        add_test(NAME agent_loop_wp5 COMMAND test_agent_loop_wp5)
        set_tests_properties(agent_loop_wp5 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skill_registry.cpp)
        add_executable(test_skill_registry tests/test_skill_registry.cpp)
        target_link_libraries(test_skill_registry PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_skill_registry PRIVATE Threads::Threads)
        endif()
        add_test(NAME skill_registry_wp18 COMMAND test_skill_registry)
        set_tests_properties(skill_registry_wp18 PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skill_cursor_bundle.cpp
       AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/data/skills-cursor)
        add_executable(test_skill_cursor_bundle tests/test_skill_cursor_bundle.cpp)
        target_link_libraries(test_skill_cursor_bundle PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_skill_cursor_bundle PRIVATE Threads::Threads)
        endif()
        target_compile_definitions(test_skill_cursor_bundle PRIVATE
            "AGENT_TEST_SKILLS_CURSOR_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/data/skills-cursor\"")
        add_test(NAME skill_cursor_bundle COMMAND test_skill_cursor_bundle)
        set_tests_properties(skill_cursor_bundle PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_prompt_renderer_skill_block.cpp)
        add_executable(test_prompt_renderer_skill_block tests/test_prompt_renderer_skill_block.cpp)
        target_link_libraries(test_prompt_renderer_skill_block PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_prompt_renderer_skill_block PRIVATE Threads::Threads)
        endif()
        add_test(NAME prompt_renderer_skill_block COMMAND test_prompt_renderer_skill_block)
        set_tests_properties(prompt_renderer_skill_block PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_skill_script_tool.cpp)
        add_executable(test_skill_script_tool tests/test_skill_script_tool.cpp)
        target_link_libraries(test_skill_script_tool PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_skill_script_tool PRIVATE Threads::Threads)
        endif()
        add_test(NAME skill_script_tool_wp18 COMMAND test_skill_script_tool)
        set_tests_properties(skill_script_tool_wp18 PROPERTIES
            ENVIRONMENT "AGENT_TOOL_ALLOWLIST=;AGENT_SKILL_SCRIPT_ALLOWLIST=/bin/sh")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fs_tools.cpp)
        add_executable(test_fs_tools tests/test_fs_tools.cpp)
        target_link_libraries(test_fs_tools PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_fs_tools PRIVATE Threads::Threads)
        endif()
        add_test(NAME fs_tools_builtin COMMAND test_fs_tools)
        set_tests_properties(fs_tools_builtin PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_web_tools.cpp)
        add_executable(test_web_tools tests/test_web_tools.cpp)
        target_link_libraries(test_web_tools PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_web_tools PRIVATE Threads::Threads)
        endif()
        if(OpenSSL_FOUND)
            target_link_libraries(test_web_tools PRIVATE OpenSSL::SSL OpenSSL::Crypto)
            target_compile_definitions(test_web_tools PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
        endif()
        if(ZLIB_FOUND)
            target_link_libraries(test_web_tools PRIVATE ZLIB::ZLIB)
            target_compile_definitions(test_web_tools PRIVATE AGENT_HAVE_ZLIB=1)
        endif()
        target_compile_definitions(test_web_tools PRIVATE
            "AGENT_TEST_WEB_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data\"")
        add_test(NAME web_tools_builtin COMMAND test_web_tools)
        set_tests_properties(web_tools_builtin PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_news_sources.cpp)
        add_executable(test_news_sources tests/test_news_sources.cpp)
        target_link_libraries(test_news_sources PRIVATE agent_framework)
        if(CMAKE_USE_PTHREADS_INIT)
            target_link_libraries(test_news_sources PRIVATE Threads::Threads)
        endif()
        if(OpenSSL_FOUND)
            target_link_libraries(test_news_sources PRIVATE OpenSSL::SSL OpenSSL::Crypto)
            target_compile_definitions(test_news_sources PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
        endif()
        target_compile_definitions(test_news_sources PRIVATE
            "AGENT_TEST_NEWS_JSON=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/news_sources_test_catalog.json\"")
        add_test(NAME news_sources_catalog COMMAND test_news_sources)
        set_tests_properties(news_sources_catalog PROPERTIES ENVIRONMENT "AGENT_TOOL_ALLOWLIST=")
    endif()
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

