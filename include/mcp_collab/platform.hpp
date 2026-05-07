#pragma once

#include <string>
#include <filesystem>

#ifdef _WIN32
    #define MCP_PLATFORM_WINDOWS 1
    #include <windows.h>
    #include <processthreadsapi.h>
#elif defined(__APPLE__)
    #define MCP_PLATFORM_MACOS 1
    #include <mach-o/dyld.h>
#elif defined(__linux__)
    #define MCP_PLATFORM_LINUX 1
    #include <unistd.h>
    #include <linux/limits.h>
#endif

namespace mcp_collab::platform {

inline std::string hostname() {
    char buf[256]{};
#ifdef MCP_PLATFORM_WINDOWS
    DWORD size = sizeof(buf);
    GetComputerNameA(buf, &size);
#else
    gethostname(buf, sizeof(buf));
#endif
    return std::string(buf);
}

inline std::string pid() {
#ifdef MCP_PLATFORM_WINDOWS
    return std::to_string(GetCurrentProcessId());
#else
    return std::to_string(getpid());
#endif
}

inline std::filesystem::path exe_path() {
#ifdef MCP_PLATFORM_WINDOWS
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf);
#elif defined(MCP_PLATFORM_MACOS)
    char buf[PATH_MAX]{};
    uint32_t size = PATH_MAX;
    _NSGetExecutablePath(buf, &size);
    return std::filesystem::path(buf);
#else
    char buf[PATH_MAX]{};
    ssize_t len = readlink("/proc/self/exe", buf, PATH_MAX - 1);
    if (len > 0) buf[len] = '\0';
    return std::filesystem::path(buf);
#endif
}

inline std::string shell_name() {
#ifdef MCP_PLATFORM_WINDOWS
    return "powershell";
#else
    const char* sh = std::getenv("SHELL");
    return sh ? std::filesystem::path(sh).filename().string() : "bash";
#endif
}

inline std::string path_separator() {
#ifdef MCP_PLATFORM_WINDOWS
    return ";";
#else
    return ":";
#endif
}

}