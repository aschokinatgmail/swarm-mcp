# swarm-mcp

A C++23 MCP (Model Context Protocol) collaboration server for multi-agent coordination using MQTT pub/sub and Git branch-per-task workflows.

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  MCP Client                      │
│            (Streamable HTTP / SSE)               │
└───────────────────┬─────────────────────────────┘
                    │  JSON-RPC 2.0
┌───────────────────▼─────────────────────────────┐
│              Swarm MCP Server                    │
│  ┌──────────┐ ┌──────────┐ ┌──────────────────┐ │
│  │ Protocol │ │ Auth/RBAC│ │ 15 MCP Tools     │ │
│  │ Handler  │ │ (3 roles)│ │ 6 Resources      │ │
│  └──────────┘ └──────────┘ │ 4 Prompts         │ │
│  ┌──────────┐ ┌──────────┐ └──────────────────┘ │
│  │ EventBus │ │TaskMgr   │ ┌──────────────────┐ │
│  └──────────┘ └──────────┘ │ AgentRegistry    │ │
│  ┌──────────┐ ┌──────────┐ │ ContextStore     │ │
│  │ChannelMgr│ │ GitOps   │ └──────────────────┘ │
│  └──────────┘ └──────────┘ ┌──────────────────┐ │
│                            │ BranchManager    │ │
│  MQTT Pub/Sub ◄──────────►│ MergeCoordinator │ │
│  (HMAC-signed envelopes)  └──────────────────┘ │
└─────────────────────────────────────────────────┘
                    │
          ┌─────────▼─────────┐
          │   Git Repository  │
          │  (branch-per-task)│
          └───────────────────┘
```

## Features

- **MCP Protocol**: Full JSON-RPC 2.0 implementation with tools, resources, prompts, and SSE notifications
- **Multi-Agent Coordination**: Agent registry, task management, context sharing via event bus
- **MQTT Pub/Sub**: HMAC-SHA256 signed envelopes with replay protection and swarm isolation
- **Git Workflows**: Branch-per-task model with lock/unlock, merge requests, and conflict detection
- **RBAC Auth**: 3 roles (Coordinator, Worker, Observer) with 19 permissions, Bearer tokens
- **Streamable HTTP**: POST for requests, GET for SSE event stream, DELETE for session teardown
- **Cross-Platform**: Windows (MSVC), Linux (GCC/Clang), macOS (Clang)

## Prerequisites

- C++23 compiler (MSVC 19.50+, GCC 13+, Clang 17+)
- CMake 3.22+
- vcpkg (for dependency management)
- Git (for branch-per-task workflows)
- MQTT broker (e.g., Mosquitto) — optional, for multi-process coordination

## Dependencies (via vcpkg)

| Library | Version | Purpose |
|---------|---------|---------|
| fmt | 12.1.0 | String formatting |
| nlohmann-json | 3.12.0 | JSON handling |
| cpp-httplib | 0.43.3 | HTTP server/client |
| spdlog | 1.17.0 | Logging |
| eclipse-paho-mqtt-c | 1.3.16 | MQTT client |
| OpenSSL | 3.6.2 | TLS, HMAC, crypto |
| GTest | 1.17.0 | Unit/integration testing |

## Build Instructions

### Windows (Visual Studio + vcpkg)

```bash
# Configure (vcpkg integrated with VS)
cmake -B build -G "Visual Studio 18 2026" -DCMAKE_TOOLCHAIN_FILE="C:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Release

# Run tests
.\build\Release\swarm_mcp_unit_tests.exe
```

### Linux / macOS

```bash
# Install vcpkg (if not already)
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh

# Configure
cmake -B build -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build -j$(nproc)

# Run tests
./build/swarm_mcp_unit_tests
```

### Running with CTest

```bash
cd build
ctest --output-on-failure -j4
```

Note: Integration tests that start HTTP servers are excluded by default. Run them individually if needed.

## Usage

### Start the Server

```bash
# With default config
./swarm-mcp

# With custom config
./swarm-mcp -c config/production.json

# Dev mode (no auth)
./swarm-mcp --no-auth -v

# Custom HTTP and MQTT
./swarm-mcp -H 0.0.0.0 -p 8080 -m broker.example.com -M 1883
```

### Enroll an Agent

```bash
./swarm-mcp --enroll agent-1 --role worker -k my-secret-key
# Outputs a Bearer token for the agent
```

### MCP Protocol Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/mcp` | Send JSON-RPC request or batch |
| GET | `/mcp` | Open SSE event stream |
| DELETE | `/mcp` | Close SSE session |
| GET | `/health` | Health check |

### Available MCP Tools (15)

`task_create`, `task_update_status`, `task_assign`, `task_list`, `task_info`, `agent_register`, `agent_deregister`, `agent_heartbeat`, `agent_list`, `branch_create`, `branch_commit`, `branch_merge_request`, `branch_list`, `context_set`, `context_get`

### Configuration

Config precedence: **CLI args > environment variables > config file > defaults**

Environment variables: `SWARM_ID`, `SWARM_SECRET`, `MQTT_HOST`, `MQTT_PORT`, `HTTP_HOST`, `HTTP_PORT`, `GIT_REPO_PATH`

See `config/default.json` for all options.

## Project Structure

```
swarm-mcp/
├── include/mcp_collab/     # Public headers
│   ├── protocol.hpp        # MCP JSON-RPC handler
│   ├── auth.hpp            # RBAC + HMAC auth
│   ├── task_manager.hpp    # Task CRUD + dependencies
│   ├── agent_registry.hpp  # Agent lifecycle management
│   ├── context_store.hpp   # Key-value context sharing
│   ├── event_bus.hpp       # Pub/sub event system
│   ├── mqtt_client.hpp     # MQTT client wrapper
│   ├── channel.hpp         # Topic-based channels
│   ├── git_operations.hpp  # Git CLI wrapper
│   ├── branch_manager.hpp  # Branch-per-task management
│   ├── merge_coordinator.hpp # Merge request workflow
│   ├── transport_http.hpp  # Streamable HTTP + SSE
│   └── config.hpp          # Configuration management
├── src/
│   ├── main.cpp            # CLI entry point
│   ├── mcp/                # Protocol, tools, resources, prompts
│   ├── mqtt/               # MQTT client, channel, secure MQTT
│   ├── workspace/          # TaskManager, AgentRegistry, ContextStore, EventBus
│   ├── git/                # GitOperations, BranchManager, MergeCoordinator
│   ├── http/               # HTTP handler with SSE
│   └── util/               # Auth, config
├── tests/
│   ├── unit/               # 11 unit test files (181 tests)
│   └── integration/        # 4 integration test files
├── config/default.json     # Default configuration
├── CMakeLists.txt          # Build system
└── vcpkg.json              # Dependency manifest
```

## Testing

- **181 unit tests** covering all core modules
- **4 integration test files** for MCP/HTTP, workspace, MQTT, and Git workflows
- Build target: `swarm_mcp_unit_tests`, `swarm_mcp_integration_tests`

## License

See [LICENSE](LICENSE) for details.
