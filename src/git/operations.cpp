#include "mcp_collab/git_operations.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#else
#include <unistd.h>
#include <sys/select.h>
#include <sys/wait.h>
#endif

namespace mcp_collab {

GitOperations::GitOperations(const std::string& repo_path)
    : repo_path_(repo_path) {}

GitResult GitOperations::exec(const std::string& args) const {
    std::lock_guard lock(exec_mutex_);  // Serialize all git operations
    GitResult result;
    std::string cmd = std::format("git -C \"{}\" {}", repo_path_, args);

    // Validate: reject shell metacharacters to prevent injection
    static const std::string dangerous_chars = "&|;`(){}!\n\r";
    for (size_t i = 0; i < args.size(); ++i) {
        if (dangerous_chars.find(args[i]) != std::string::npos) {
            spdlog::error("Shell injection attempt rejected in git args: {}", args);
            result.exit_code = -1;
            return result;
        }
        if (args[i] == '$') {
            int quote_count = 0;
            for (size_t j = 0; j < i; ++j) {
                if (args[j] == '"') quote_count++;
            }
            if (quote_count % 2 == 0) {
                spdlog::error("Unquoted shell variable in git args: {}", args);
                result.exit_code = -1;
                return result;
            }
        }
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE h_out_read, h_out_write, h_err_read, h_err_write;
    CreatePipe(&h_out_read, &h_out_write, &sa, 0);
    CreatePipe(&h_err_read, &h_err_write, &sa, 0);
    SetHandleInformation(h_out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(STARTUPINFOA)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out_write;
    si.hStdError = h_err_write;
    PROCESS_INFORMATION pi{};

    char buf[4096];
    snprintf(buf, sizeof(buf), "cmd /c %s", cmd.c_str());
    CreateProcessA(nullptr, buf, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);

    CloseHandle(h_out_write);
    CloseHandle(h_err_write);

    char read_buf[4096];
    DWORD bytes_read;
    while (ReadFile(h_out_read, read_buf, sizeof(read_buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        result.stdout_out.append(read_buf, bytes_read);
    }
    while (ReadFile(h_err_read, read_buf, sizeof(read_buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        result.stderr_out.append(read_buf, bytes_read);
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    result.success = (exit_code == 0);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(h_out_read);
    CloseHandle(h_err_read);
#else
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        result.success = false;
        return result;
    }

    fd_set read_fds;
    struct timeval tv;
    char buffer[4096];
    bool timed_out = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) { timed_out = true; break; }

        int fd = fileno(pipe);
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        tv.tv_sec = static_cast<long>(remaining.count()) / 1000;
        tv.tv_usec = (static_cast<long>(remaining.count()) % 1000) * 1000;

        int sel = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) { timed_out = true; break; }

        size_t n = fread(buffer, 1, sizeof(buffer), pipe);
        if (n == 0) break;
        result.stdout_out.append(buffer, n);
    }

    int status = pclose(pipe);
    if (timed_out) {
        result.success = false;
        result.exit_code = -1;
        result.stderr_out = "git operation timed out after 30 seconds";
    } else {
        result.exit_code = WEXITSTATUS(status);
        result.success = (result.exit_code == 0);
    }
#endif

    spdlog::debug("git {}: exit={} out_len={}", args, result.exit_code, result.stdout_out.size());
    return result;
}

bool GitOperations::is_repo() const {
    return exec("rev-parse --is-inside-work-tree").success;
}

std::string GitOperations::current_branch() const {
    auto r = exec("rev-parse --abbrev-ref HEAD");
    if (!r.success) return "";
    auto& s = r.stdout_out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string GitOperations::current_commit() const {
    auto r = exec("rev-parse HEAD");
    if (!r.success) return "";
    auto& s = r.stdout_out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool GitOperations::has_changes() const {
    auto r = exec("status --porcelain");
    return r.success && !r.stdout_out.empty();
}

bool GitOperations::init() const { return exec("init --initial-branch=main").success; }
bool GitOperations::fetch() const { return exec("fetch --all").success; }
bool GitOperations::pull() const { return exec("pull --rebase").success; }
bool GitOperations::push() const { return exec("push").success; }

bool GitOperations::commit(const std::string& message, const std::string& author) const {
    std::string args = std::format("commit -m \"{}\"", message);
    if (!author.empty()) args += std::format(" --author=\"{} <{}>\"", author, author);
    return exec(args).success;
}

bool GitOperations::add(const std::string& pathspec) const {
    return exec(std::format("add {}", pathspec)).success;
}

bool GitOperations::checkout(const std::string& branch, bool create) const {
    std::string args = create ? std::format("checkout -b {}", branch) : std::format("checkout {}", branch);
    return exec(args).success;
}

bool GitOperations::merge(const std::string& branch, bool no_ff) const {
    std::string args = no_ff ? std::format("merge --no-ff {}", branch) : std::format("merge {}", branch);
    return exec(args).success;
}

bool GitOperations::merge_squash(const std::string& branch) const {
    return exec(std::format("merge --squash {}", branch)).success;
}

bool GitOperations::rebase(const std::string& branch) const {
    return exec(std::format("rebase {}", branch)).success;
}

bool GitOperations::branch_delete(const std::string& branch, bool force) const {
    std::string flag = force ? "-D" : "-d";
    return exec(std::format("branch {} {}", flag, branch)).success;
}

bool GitOperations::stash() const { return exec("stash").success; }
bool GitOperations::stash_pop() const { return exec("stash pop").success; }

bool GitOperations::reset(const std::string& ref, bool hard) const {
    std::string flag = hard ? "--hard" : "--soft";
    return exec(std::format("reset {} {}", flag, ref)).success;
}

bool GitOperations::cherry_pick(const std::string& commit_hash) const {
    return exec(std::format("cherry-pick {}", commit_hash)).success;
}

std::vector<std::string> GitOperations::log(int count, const std::string& format) const {
    std::string fmt = format.empty() ? "--oneline" : std::format("--format={}", format);
    auto r = exec(std::format("log {} -{}", fmt, count));
    if (!r.success) return {};

    std::vector<std::string> lines;
    std::istringstream iss(r.stdout_out);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> GitOperations::branches(bool remote) const {
    std::string args = remote ? "branch -r" : "branch";
    auto r = exec(args);
    if (!r.success) return {};

    std::vector<std::string> result;
    std::istringstream iss(r.stdout_out);
    std::string line;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        auto trimmed = line;
        auto start = trimmed.find_first_not_of(" \t*");
        if (start != std::string::npos) {
            result.push_back(trimmed.substr(start));
        }
    }
    return result;
}

std::string GitOperations::diff(const std::string& from_ref, const std::string& to_ref) const {
    std::string args;
    if (!from_ref.empty() && !to_ref.empty()) {
        args = std::format("diff {}...{}", from_ref, to_ref);
    } else if (!from_ref.empty()) {
        args = std::format("diff {}", from_ref);
    } else {
        args = "diff";
    }
    auto r = exec(args);
    return r.success ? r.stdout_out : "";
}

std::string GitOperations::show(const std::string& ref) const {
    auto r = exec(std::format("show {}", ref));
    return r.success ? r.stdout_out : "";
}

}