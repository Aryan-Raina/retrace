// retrace -- a deterministic record-replay debugger for single-threaded
// x86-64 Linux programs.

#include "log.hpp"
#include "replay.hpp"
#include "retrace.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// env(1) convention: the target's own status is passed through, so retrace
// reserves this band for its own failures and always explains itself.
constexpr int kInternal = 125;
constexpr int kNotExecutable = 126;
constexpr int kNotFound = 127;

constexpr std::string_view kUsage =
    "usage: retrace <command> [options] -- <program-path> [arguments...]\n"
    "\n"
    "  run              launch the program and supervise it\n"
    "  record [-o LOG]  the same, reporting every system-call entry and exit,\n"
    "                   optionally writing them to a log\n"
    "  show LOG         print a recorded log\n"
    "  replay LOG       re-run the program against a recording, feeding back\n"
    "                   the recorded results and reporting any divergence\n"
    "\n"
    "  replay --break 0xADDR LOG   stop each time that address is executed;\n"
    "                              then c = continue, rc = reverse-continue,\n"
    "                              r = registers, q = quit\n"
    "\n"
    "The '--' separator is required. Every token after it is passed to the\n"
    "target verbatim: no flag parsing, no shell, no expansion.\n"
    "\n"
    "PATH is not searched. Name the program by path, as './prog' or '/bin/prog',\n"
    "so that a recording always identifies exactly which binary it ran.\n";

int usage_error(const std::string& message) {
    retrace::complain(message);
    std::cerr << kUsage;
    return kInternal;
}

}  // namespace

// Prints a recorded log in the same form `record` printed it live.
int show(const char* path) {
    std::vector<retrace::Record> records;
    if (!retrace::read_log(path, &records)) return kInternal;

    for (size_t i = 0; i < records.size(); ++i) {
        std::cout << std::format("[{:4}] {}\n", i, render_call(records[i]));
        std::cout << std::format("[{:4}]   = {}\n", i, render_result(records[i]));
    }
    std::cout << std::format("{} records\n", records.size());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage_error("no command given");

    const std::string_view name = argv[1];
    retrace::Mode mode{};
    if (name == "run") {
        mode = retrace::Mode::kRun;
    } else if (name == "record") {
        mode = retrace::Mode::kRecord;
    } else if (name == "replay") {
        mode = retrace::Mode::kReplay;
    } else if (name == "show") {
        if (argc < 3) return usage_error("show needs a log file");
        return show(argv[2]);
    } else {
        return usage_error(std::format("unknown subcommand '{}'", name));
    }

    // Options and the log path come before '--'.
    const char* log_path = nullptr;
    int i = 2;
    unsigned long break_at = 0;
    if (mode == retrace::Mode::kReplay) {
        if (argc > 2 && std::string_view(argv[2]) == "--break") {
            if (argc < 4) return usage_error("--break needs an address");
            break_at = std::strtoul(argv[3], nullptr, 0);
            if (break_at == 0) {
                return usage_error(
                    std::format("'{}' is not an address", argv[3]));
            }
            i = 4;
        }
        if (i >= argc) return usage_error("replay needs a log file");
        log_path = argv[i];
        ++i;
    }
    while (i < argc && std::string_view(argv[i]) != "--") {
        if (std::string_view(argv[i]) == "-o" && mode == retrace::Mode::kRecord) {
            if (i + 1 >= argc) return usage_error("-o needs a file name");
            log_path = argv[i + 1];
            i += 2;
            continue;
        }
        return usage_error(
            std::format("expected '--' before the program, found '{}'", argv[i]));
    }

    if (i >= argc) {
        return usage_error(i == 2 ? "no program specified"
                                  : "no program specified after '--'");
    }
    ++i;  // step past '--'
    if (i >= argc) return usage_error("no program specified after '--'");
    const int first = i;

    // PATH is never searched: which binary a bare name resolves to depends on
    // the environment, and a recording has to name its target exactly.
    const std::string_view program = argv[first];
    if (program.find('/') == std::string_view::npos) {
        return usage_error(std::format(
            "'{}' is not a path: retrace does not search PATH, so name the "
            "program as './{}' or by its absolute path",
            program, program));
    }

    // argv[argc] is NULL, so the tail from argv[3] is already a well-formed
    // argument vector and can go straight to execv.
    std::vector<retrace::Record> records;
    if (mode == retrace::Mode::kReplay &&
        !retrace::read_log(log_path, &records)) {
        return kInternal;
    }

    retrace::LogWriter log;
    if (mode == retrace::Mode::kRecord && log_path != nullptr &&
        !log.open(log_path)) {
        return kInternal;
    }

    // Reverse-continue cannot rewind a running process, so it relaunches and
    // replays to an earlier breakpoint hit. Deterministic replay is what makes
    // the second run stop in the same place.
    if (mode == retrace::Mode::kReplay) {
        retrace::ReplayRequest request{break_at, 1};
        for (;;) {
            const retrace::Launch target = retrace::launch(&argv[first]);
            if (target.failed_call != nullptr || target.exec_errno != 0) {
                retrace::complain("could not relaunch the target");
                return kInternal;
            }
            const retrace::ReplayOutcome outcome =
                retrace::replay(target.pid, records, request);
            if (!outcome.restart) return outcome.code;
            request.stop_at = outcome.stop_at;
        }
    }

    const retrace::Launch launched = retrace::launch(&argv[first]);

    if (launched.failed_call != nullptr) {
        retrace::complain(std::format("{} failed: {}", launched.failed_call,
                                      std::strerror(launched.internal_errno)));
        if (launched.internal_errno == EPERM) {
            retrace::complain("check kernel.yama.ptrace_scope, or a container "
                              "seccomp policy blocking ptrace");
        }
        return kInternal;
    }

    if (launched.exec_errno != 0) {
        retrace::complain(
            std::format("{}: {}", argv[first], std::strerror(launched.exec_errno)));
        return (launched.exec_errno == ENOENT || launched.exec_errno == ENOTDIR)
                   ? kNotFound
                   : kNotExecutable;
    }

    const int code = retrace::supervise(launched.pid, mode,
                                        log_path != nullptr ? &log : nullptr);
    log.close();
    return code;
}
