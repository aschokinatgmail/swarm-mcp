#include "mcp_collab/git_operations.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <csignal>

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

// Primary vector-based exec: fork+execvp, no shell, separate stdout/stderr capture, 30s timeout
GitResult GitOperations::exec(std::vector<std::string> argv) const {
    std::lock_guard lock(exec_mutex_);
    GitResult result;

#ifdef _WIN32
    // Windows: use CreateProcess with argv (no shell)
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

    // Build command line: git -C <repo> <argv...>
    // Windows quoting: wrap each arg in double quotes, escape embedded
    // double quotes with backslashes (Windows command-line convention).
    auto win_quote = [](const std::string& s) {
        std::string q = "\"";
        for (char c : s) {
            if (c == '"') q += "\\\"";
            else q += c;
        }
        q += "\"";
        return q;
    };
    std::string cmdline = "git";
    cmdline += " -C " + win_quote(repo_path_);
    for (const auto& a : argv) {
        cmdline += " " + win_quote(a);
    }

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", cmdline.c_str());
    if (!CreateProcessA(nullptr, buf, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        result.success = false;
        result.exit_code = 127;
        CloseHandle(h_out_read); CloseHandle(h_out_write);
        CloseHandle(h_err_read); CloseHandle(h_err_write);
        return result;
    }

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
    // POSIX: fork + execvp with explicit argv, no shell
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        result.success = false;
        result.exit_code = -1;
        result.stderr_out = "pipe() failed";
        return result;
    }

    // Build argv for execvp: {"git", "-C", repo_path_, <argv...>, nullptr}
    std::vector<char*> exec_argv;
    exec_argv.push_back(const_cast<char*>("git"));
    exec_argv.push_back(const_cast<char*>("-C"));
    exec_argv.push_back(const_cast<char*>(repo_path_.c_str()));
    for (const auto& a : argv) {
        exec_argv.push_back(const_cast<char*>(a.c_str()));
    }
    exec_argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        result.success = false;
        result.exit_code = -1;
        result.stderr_out = "fork() failed";
        return result;
    }

    if (pid == 0) {
        // Child: redirect stdout/stderr, exec
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execvp("git", exec_argv.data());
        _exit(127);  // execvp failed
    }

    // Parent: close write ends, read stdout & stderr with poll, enforce 30s timeout
    close(out_pipe[1]);
    close(err_pipe[1]);

    char buffer[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timed_out = false;

    bool out_done = false, err_done = false;
    while (!(out_done && err_done)) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            timed_out = true;
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int maxfd = -1;
        if (!out_done) { FD_SET(out_pipe[0], &read_fds); maxfd = std::max(maxfd, out_pipe[0]); }
        if (!err_done) { FD_SET(err_pipe[0], &read_fds); maxfd = std::max(maxfd, err_pipe[0]); }

        struct timeval tv;
        tv.tv_sec = static_cast<long>(remaining.count()) / 1000;
        tv.tv_usec = (static_cast<long>(remaining.count()) % 1000) * 1000;

        int sel = select(maxfd + 1, &read_fds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) {
            timed_out = true;
            break;
        }

        if (!out_done && FD_ISSET(out_pipe[0], &read_fds)) {
            ssize_t n = read(out_pipe[0], buffer, sizeof(buffer));
            if (n > 0) result.stdout_out.append(buffer, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else out_done = true;
        }
        if (!err_done && FD_ISSET(err_pipe[0], &read_fds)) {
            ssize_t n = read(err_pipe[0], buffer, sizeof(buffer));
            if (n > 0) result.stderr_out.append(buffer, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else err_done = true;
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        result.success = false;
        result.exit_code = -1;
        result.stderr_out = "git operation timed out after 30 seconds";
    }

    int status;
    waitpid(pid, &status, 0);
    if (!timed_out) {
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        result.success = (result.exit_code == 0);
    }
#endif

    // Build a debug string from argv for logging
    std::string args_debug;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) args_debug += " ";
        args_debug += argv[i];
    }
    spdlog::debug("git {}: exit={} out_len={} err_len={}", args_debug, result.exit_code, result.stdout_out.size(), result.stderr_out.size());
    return result;
}

// Helper: tokenize a shell-like string into argv, respecting single/double quotes
// Used only by the backward-compat string overload.
static std::vector<std::string> tokenize_args(const std::string& args) {
    std::vector<std::string> result;
    std::string current;
    char quote = 0;  // 0 = none, '"' or '\''
    size_t i = 0;
    while (i < args.size()) {
        char c = args[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else {
                current += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
        ++i;
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

// Backward-compat string overload: tokenizes safely, delegates to vector version
GitResult GitOperations::exec_str(const std::string& args) const {
    return exec(tokenize_args(args));
}

bool GitOperations::is_repo() const {
    auto r = exec({"rev-parse", "--show-toplevel"});
    if (!r.success) return false;
    auto& s = r.stdout_out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return std::filesystem::weakly_canonical(s) == std::filesystem::weakly_canonical(repo_path_);
}

std::string GitOperations::current_branch() const {
    auto r = exec({"rev-parse", "--abbrev-ref", "HEAD"});
    if (!r.success) return "";
    auto& s = r.stdout_out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string GitOperations::current_commit() const {
    auto r = exec({"rev-parse", "HEAD"});
    if (!r.success) return "";
    auto& s = r.stdout_out;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

bool GitOperations::has_changes() const {
    auto r = exec({"status", "--porcelain"});
    return r.success && !r.stdout_out.empty();
}

bool GitOperations::init() const { return exec({"init", "--initial-branch=main"}).success; }
bool GitOperations::fetch() const { return exec({"fetch", "--all"}).success; }
bool GitOperations::pull() const { return exec({"pull", "--rebase"}).success; }
bool GitOperations::push() const { return exec({"push"}).success; }

bool GitOperations::commit(const std::string& message, const std::string& author) const {
    std::vector<std::string> argv = {"commit", "-m", message};
    if (!author.empty()) {
        argv.push_back("--author");
        argv.push_back(author + " <" + author + ">");
    }
    return exec(argv).success;
}

bool GitOperations::add(const std::string& pathspec) const {
    return exec({"add", pathspec}).success;
}

bool GitOperations::checkout(const std::string& branch, bool create) const {
    if (create) {
        return exec({"checkout", "-b", branch}).success;
    } else {
        return exec({"checkout", branch}).success;
    }
}

bool GitOperations::merge(const std::string& branch, bool no_ff) const {
    if (no_ff) {
        return exec({"merge", "--no-ff", branch}).success;
    } else {
        return exec({"merge", branch}).success;
    }
}

bool GitOperations::merge_squash(const std::string& branch) const {
    return exec({"merge", "--squash", branch}).success;
}

bool GitOperations::rebase(const std::string& branch) const {
    return exec({"rebase", branch}).success;
}

bool GitOperations::branch_delete(const std::string& branch, bool force) const {
    return exec({"branch", force ? "-D" : "-d", branch}).success;
}

bool GitOperations::stash() const { return exec({"stash"}).success; }
bool GitOperations::stash_pop() const { return exec({"stash", "pop"}).success; }

bool GitOperations::reset(const std::string& ref, bool hard) const {
    return exec({"reset", hard ? "--hard" : "--soft", ref}).success;
}

bool GitOperations::cherry_pick(const std::string& commit_hash) const {
    return exec({"cherry-pick", commit_hash}).success;
}

std::vector<std::string> GitOperations::log(int count, const std::string& format) const {
    std::vector<std::string> argv = {"log"};
    if (format.empty()) {
        argv.push_back("--oneline");
    } else {
        argv.push_back("--format=" + format);
    }
    argv.push_back("-" + std::to_string(count));
    auto r = exec(argv);
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
    std::vector<std::string> argv = {"branch"};
    if (remote) argv.push_back("-r");
    auto r = exec(argv);
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
    std::vector<std::string> argv = {"diff"};
    if (!from_ref.empty() && !to_ref.empty()) {
        argv.push_back(from_ref + "..." + to_ref);
    } else if (!from_ref.empty()) {
        argv.push_back(from_ref);
    }
    auto r = exec(argv);
    return r.success ? r.stdout_out : "";
}

std::string GitOperations::show(const std::string& ref) const {
    auto r = exec({"show", ref});
    return r.success ? r.stdout_out : "";
}

}