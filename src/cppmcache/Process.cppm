// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Process.cppm
 * @brief Launching of the compiler (POSIX variant only for now).
 */
module;

#ifdef _WIN32
    #error "Process launching on Windows remains to be written (MSVC milestone)."
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

export module cppmcache.process;

import std;

namespace fs = std::filesystem;

namespace cppmcache {

std::vector<char*> toArgv(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

}  // namespace cppmcache

export namespace cppmcache {

// Looks for an executable as the shell would: as is if it contains a separator, otherwise
// in the directories of PATH.
std::optional<fs::path> findProgram(const std::string& name) {
    std::error_code error;
    if (name.find('/') != std::string::npos) {
        return fs::exists(name, error) ? std::optional<fs::path>{name} : std::nullopt;
    }
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return std::nullopt;
    }
    for (const auto directory : std::views::split(std::string_view{path}, ':')) {
        const fs::path candidate =
            fs::path{
                std::string_view{directory.begin(), directory.end()}
        }
            / name;
        if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

// Replaces the current process with the command. Returns only on failure.
int execute(const std::vector<std::string>& arguments) {
    auto argv = toArgv(arguments);
    ::execvp(argv.front(), argv.data());
    std::println(std::cerr, "cppmcache: cannot launch {}: {}", arguments.front(), std::generic_category().message(errno));
    return 127;
}

// Runs the command, redirecting its outputs to two files; returns its exit code.
int executeCaptured(const std::vector<std::string>& arguments, const fs::path& standardOutput, const fs::path& standardError) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, standardOutput.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, 2, standardError.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    auto      argv    = toArgv(arguments);
    pid_t     child   = 0;
    const int spawned = ::posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        std::println(std::cerr, "cppmcache: cannot launch {}: {}", arguments.front(), std::generic_category().message(spawned));
        return 127;
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return 127;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
}

}  // namespace cppmcache
