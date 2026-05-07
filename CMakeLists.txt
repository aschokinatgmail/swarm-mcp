cmake_minimum_required(VERSION 3.25)
project(swarm-mcp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(paho-mqtt3a CONFIG REQUIRED)
find_package(paho-mqtt3as CONFIG REQUIRED)
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
    src/util/uuid.cpp
    src/util/config.cpp
)

target_link_libraries(swarm-mcp PRIVATE
    fmt::fmt
    nlohmann_json::nlohmann_json
    paho-mqtt3a::paho-mqtt3a
    paho-mqtt3as::paho-mqtt3as
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
    target_link_libraries(swarm-mcp PRIVATE ws2_32 crypt32)
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