cmake_minimum_required(VERSION 3.22)
project(swarm-mcp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_COVERAGE "Enable code coverage reporting (GCC/Clang only)" OFF)

if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Code coverage enabled")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(eclipse-paho-mqtt-c CONFIG REQUIRED)

# Prefer static target, fall back to shared (vcpkg on Windows builds shared by default)
if(TARGET eclipse-paho-mqtt-c::paho-mqtt3as-static)
    set(PAHO_MQTT_C_TARGET eclipse-paho-mqtt-c::paho-mqtt3as-static)
elseif(TARGET eclipse-paho-mqtt-c::paho-mqtt3as)
    set(PAHO_MQTT_C_TARGET eclipse-paho-mqtt-c::paho-mqtt3as)
else()
    message(FATAL_ERROR "No suitable eclipse-paho-mqtt-c target found (tried paho-mqtt3as-static and paho-mqtt3as)")
endif()

find_package(httplib CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)

add_executable(swarm-mcp
    src/main.cpp
    src/mcp/protocol.cpp
    src/mcp/server.cpp
    src/mcp/tools.cpp
    src/mcp/resources.cpp
    src/mcp/prompts.cpp
    src/mqtt/client.cpp
    src/mqtt/channel.cpp
    src/mqtt/secure_mqtt.cpp
    src/workspace/task_manager.cpp
    src/workspace/event_bus.cpp
    src/workspace/context_store.cpp
    src/workspace/agent_registry.cpp
    src/git/operations.cpp
    src/git/branch_manager.cpp
    src/git/merge_coordinator.cpp
    src/http/handler.cpp
    src/util/auth.cpp
    src/util/config.cpp
    src/util/persistence.cpp
    src/util/keychain.cpp
)

target_link_libraries(swarm-mcp PRIVATE
    fmt::fmt
    nlohmann_json::nlohmann_json
    ${PAHO_MQTT_C_TARGET}
    httplib::httplib
    spdlog::spdlog
    OpenSSL::Crypto
)

target_include_directories(swarm-mcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(swarm-mcp PUBLIC
    $<$<CONFIG:Debug>:DEBUG>
)

if(WIN32)
    target_link_libraries(swarm-mcp PRIVATE ws2_32 crypt32 rpcrt4)
    target_compile_definitions(swarm-mcp PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(UNIX)
    target_link_libraries(swarm-mcp PRIVATE pthread)
    if(APPLE)
        target_link_libraries(swarm-mcp PRIVATE
            "-framework CoreFoundation"
            "-framework Security"
        )
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swarm-mcp PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
elseif(MSVC)
    target_compile_options(swarm-mcp PRIVATE /W4 /permissive-)
endif()

# ── Test infrastructure ────────────────────────────────────────────────
enable_testing()

find_package(GTest CONFIG REQUIRED)

# Shared library for linking both main exe and test exe
add_library(swarm_mcp_lib STATIC
    src/mcp/protocol.cpp
    src/mcp/tools.cpp
    src/mcp/resources.cpp
    src/mcp/prompts.cpp
    src/mqtt/client.cpp
    src/mqtt/channel.cpp
    src/mqtt/secure_mqtt.cpp
    src/workspace/task_manager.cpp
    src/workspace/event_bus.cpp
    src/workspace/context_store.cpp
    src/workspace/agent_registry.cpp
    src/git/operations.cpp
    src/git/branch_manager.cpp
    src/git/merge_coordinator.cpp
    src/http/handler.cpp
    src/util/auth.cpp
    src/util/config.cpp
    src/util/persistence.cpp
    src/util/keychain.cpp
)

target_link_libraries(swarm_mcp_lib PUBLIC
    fmt::fmt
    nlohmann_json::nlohmann_json
    ${PAHO_MQTT_C_TARGET}
    httplib::httplib
    spdlog::spdlog
    OpenSSL::Crypto
)

target_include_directories(swarm_mcp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(swarm_mcp_lib PUBLIC
    $<$<CONFIG:Debug>:DEBUG>
)

if(WIN32)
    target_link_libraries(swarm_mcp_lib PUBLIC ws2_32 crypt32 rpcrt4)
    target_compile_definitions(swarm_mcp_lib PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(UNIX)
    target_link_libraries(swarm_mcp_lib PUBLIC pthread)
    if(APPLE)
        target_link_libraries(swarm_mcp_lib PUBLIC
            "-framework CoreFoundation"
            "-framework Security"
        )
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swarm_mcp_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
    target_compile_options(swarm_mcp_lib PRIVATE /W4 /permissive-)
endif()

# Re-link main exe against the shared lib
target_link_libraries(swarm-mcp PRIVATE swarm_mcp_lib)

# ── Unit tests ─────────────────────────────────────────────────────────
add_executable(swarm_mcp_unit_tests
    tests/unit/test_uuid.cpp
    tests/unit/test_auth.cpp
    tests/unit/test_task_manager.cpp
    tests/unit/test_event_bus.cpp
    tests/unit/test_context_store.cpp
    tests/unit/test_agent_registry.cpp
    tests/unit/test_git_operations.cpp
    tests/unit/test_branch_manager.cpp
    tests/unit/test_merge_coordinator.cpp
    tests/unit/test_secure_mqtt.cpp
    tests/unit/test_protocol.cpp
    tests/unit/test_config.cpp
    tests/unit/test_mcp_tools.cpp
    tests/unit/test_mcp_resources.cpp
    tests/unit/test_mcp_prompts.cpp
    tests/unit/test_channel.cpp
    tests/unit/test_persistence.cpp
    tests/unit/test_http_handler.cpp
    tests/unit/test_keychain.cpp
    tests/unit/test_mqtt_client.cpp
    tests/unit/test_coverage_boost.cpp
    tests/unit/test_branch_coverage.cpp
    tests/unit/test_coverage_expansion.cpp
    tests/unit/test_coverage_gaps.cpp
)

target_link_libraries(swarm_mcp_unit_tests PRIVATE swarm_mcp_lib GTest::gtest GTest::gtest_main)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swarm_mcp_unit_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
    target_compile_options(swarm_mcp_unit_tests PRIVATE /W4 /permissive-)
endif()

include(GoogleTest)
gtest_discover_tests(swarm_mcp_unit_tests)

# ── Integration tests ──────────────────────────────────────────────────
add_executable(swarm_mcp_integration_tests
    tests/integration/test_mcp_http_integration.cpp
    tests/integration/test_workspace_integration.cpp
    tests/integration/test_mqtt_workspace_integration.cpp
    tests/integration/test_git_workflow_integration.cpp
)

target_link_libraries(swarm_mcp_integration_tests PRIVATE swarm_mcp_lib GTest::gtest GTest::gtest_main)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(swarm_mcp_integration_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
elseif(MSVC)
    target_compile_options(swarm_mcp_integration_tests PRIVATE /W4 /permissive-)
endif()

gtest_discover_tests(swarm_mcp_integration_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES RESOURCE_LOCK mcp_http_integration_port
)
