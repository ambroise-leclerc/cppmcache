// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ambroise Leclerc
/**
 * @file Process.cppm
 * @brief Launching the compiler, on POSIX systems and on Windows.
 */
module;

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <fcntl.h>
    #include <io.h>
    #include <stdio.h>
    #include <windows.h>
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

#ifdef _WIN32

// Quotes one argument so that the C runtime of the child process reads it back unchanged: the
// inverse of the rules CommandLineToArgvW applies.
std::string quoteArgument(std::string_view argument) {
    if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string{argument};
    }
    std::string quoted{'"'};
    for (std::size_t i = 0; i < argument.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == argument.size()) {
            // Backslashes before the closing quote are doubled, so that the quote stays a quote.
            quoted.append(backslashes * 2, '\\');
            break;
        }
        if (argument[i] == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
        } else {
            quoted.append(backslashes, '\\');
        }
        quoted.push_back(argument[i]);
    }
    quoted.push_back('"');
    return quoted;
}

// A handle the child process can inherit, opened for writing on a new file.
HANDLE createInheritableFile(const fs::path& path) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength        = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    return ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Runs the command with the given standard output and error handles and waits for it.
int spawnAndWait(const std::vector<std::string>& arguments, HANDLE standardOutput, HANDLE standardError) {
    std::string commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(' ');
        }
        commandLine += quoteArgument(argument);
    }
    STARTUPINFOA startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = standardOutput;
    startup.hStdError  = standardError;
    PROCESS_INFORMATION process{};
    // The program is found by CreateProcess itself, which searches the PATH and appends ".exe".
    if (!::CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        std::println(std::cerr, "cppmcache: cannot launch {}: Windows error {}", arguments.front(), ::GetLastError());
        return 127;
    }
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 127;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

#else

std::vector<char*> toArgv(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

#endif

}  // namespace cppmcache

export namespace cppmcache {

// The compiler's output is relayed byte for byte. On Windows the standard streams translate line
// endings by default, which would turn the "\r\n" a compiler wrote into "\r\r\n".
void useBinaryStandardStreams() {
#ifdef _WIN32
    ::_setmode(::_fileno(stdout), _O_BINARY);
    ::_setmode(::_fileno(stderr), _O_BINARY);
#endif
}

// Looks for an executable as the shell would: as given if it contains a separator, otherwise in
// the directories of PATH.
std::optional<fs::path> findProgram(const std::string& name) {
    std::error_code error;
#ifdef _WIN32
    const char             pathSeparator = ';';
    const std::string_view separators    = "/\\:";
    const auto             candidates    = [](const fs::path& base) {
        std::vector<fs::path> paths{base};
        if (!base.has_extension()) {
            paths.push_back(fs::path{base}.concat(".exe"));
        }
        return paths;
    };
#else
    const char             pathSeparator = ':';
    const std::string_view separators    = "/";
    const auto             candidates    = [](const fs::path& base) {
        return std::vector<fs::path>{base};
    };
#endif
    const auto firstExisting = [&](const fs::path& base) -> std::optional<fs::path> {
        for (const auto& candidate : candidates(base)) {
            if (fs::is_regular_file(candidate, error)) {
#ifndef _WIN32
                if (::access(candidate.c_str(), X_OK) != 0) {
                    continue;
                }
#endif
                return candidate;
            }
        }
        return std::nullopt;
    };
    if (name.find_first_of(separators) != std::string::npos) {
        return firstExisting(name);
    }
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return std::nullopt;
    }
    for (const auto directory : std::views::split(std::string_view{path}, pathSeparator)) {
        if (directory.empty()) {
            continue;
        }
        if (auto found = firstExisting(fs::path{
                                           std::string_view{directory.begin(), directory.end()}
        }
                                       / name)) {
            return found;
        }
    }
    return std::nullopt;
}

// Runs the command in place of the current process and returns its exit code. On POSIX systems the
// process is replaced, so the function only returns on failure.
int execute(const std::vector<std::string>& arguments) {
#ifdef _WIN32
    return spawnAndWait(arguments, ::GetStdHandle(STD_OUTPUT_HANDLE), ::GetStdHandle(STD_ERROR_HANDLE));
#else
    auto argv = toArgv(arguments);
    ::execvp(argv.front(), argv.data());
    std::println(std::cerr, "cppmcache: cannot launch {}: {}", arguments.front(), std::generic_category().message(errno));
    return 127;
#endif
}

// Runs the command with its outputs redirected to two files; returns its exit code.
int executeCaptured(const std::vector<std::string>& arguments, const fs::path& standardOutput, const fs::path& standardError) {
#ifdef _WIN32
    const HANDLE output = createInheritableFile(standardOutput);
    const HANDLE error  = createInheritableFile(standardError);
    if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE) {
        std::println(std::cerr, "cppmcache: cannot create the capture files: Windows error {}", ::GetLastError());
        if (output != INVALID_HANDLE_VALUE) {
            ::CloseHandle(output);
        }
        if (error != INVALID_HANDLE_VALUE) {
            ::CloseHandle(error);
        }
        return 127;
    }
    const int exitCode = spawnAndWait(arguments, output, error);
    ::CloseHandle(output);
    ::CloseHandle(error);
    return exitCode;
#else
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
#endif
}

}  // namespace cppmcache
