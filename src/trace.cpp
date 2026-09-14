// The wait loop, in both modes.
//
// run mode resumes with PTRACE_CONT; record mode resumes with PTRACE_SYSCALL,
// which makes the kernel stop the tracee on both sides of every syscall.

#include "log.hpp"
#include "retrace.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <string>

#include <csignal>
#include <sys/wait.h>

namespace retrace {
namespace {

constexpr int kExitInternal = 125;

std::string signal_name(int signal) {
    const char* abbrev = sigabbrev_np(signal);
    return abbrev != nullptr ? std::format("SIG{}", abbrev) : "unknown";
}

int abandon(pid_t pid, const std::string& message) {
    kill_and_reap(pid);
    complain(message);
    return kExitInternal;
}

// After a syscall that fills a caller-supplied buffer, copy what the kernel
// wrote out of the tracee. The return value is the byte count.
void capture_buffer(pid_t pid, Record* record) {
    const int arg = buffer_arg_of(record->head.nr);
    if (arg < 0 || record->head.ret <= 0) return;

    const auto length = static_cast<size_t>(record->head.ret);
    const auto address = static_cast<unsigned long>(record->head.args[arg]);
    record->buf.assign(length, '\0');
    if (!read_memory(pid, address, record->buf.data(), length)) {
        record->buf.clear();
        return;
    }
    record->head.buf_len = static_cast<uint32_t>(length);
    record->head.buf_arg = arg;
}

}  // namespace

int supervise(pid_t pid, Mode mode, LogWriter* log) {
    const bool tracing = mode == Mode::kRecord;
    const auto go = [&](int signal) {
        return tracing ? resume_to_syscall(pid, signal) : resume(pid, signal);
    };

    // launch() hands the target back stopped at its entry point.
    if (!go(0)) {
        return abandon(pid, std::format("ptrace resume failed: {}", std::strerror(errno)));
    }

    // The kernel does not label a syscall stop as entry or exit; they simply
    // alternate, and the first one we see is an entry because the target had
    // not begun a syscall when launch() handed it over.
    bool at_entry = true;
    Record record;
    unsigned long index = 0;

    for (;;) {
        int status = 0;
        if (!wait_for(pid, &status)) {
            return abandon(pid, std::format("waitpid failed: {}", std::strerror(errno)));
        }

        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) {
            const int signal = WTERMSIG(status);
            complain(std::format("terminated by signal {} ({}){}", signal,
                                 signal_name(signal),
                                 WCOREDUMP(status) ? ", core dumped" : ""));
            return 128 + signal;
        }
        if (!WIFSTOPPED(status)) {
            return abandon(pid, std::format("unexpected wait status 0x{:04x}",
                                            static_cast<unsigned>(status)));
        }

        int signal = WSTOPSIG(status);

        if (tracing && signal == (SIGTRAP | 0x80)) {
            Registers regs{};
            if (!get_regs(pid, &regs)) {
                return abandon(pid, std::format("ptrace(PTRACE_GETREGS) failed: {}",
                                                std::strerror(errno)));
            }

            if (at_entry) {
                record = Record{};
                record.head.nr = syscall_nr(regs);
                record.head.buf_arg = -1;
                for (int i = 0; i < 6; ++i) {
                    record.head.args[i] = syscall_arg(regs, i);
                }
                std::cerr << std::format("[{:4}] {}\n", index, render_call(record));
            } else {
                record.head.ret = syscall_ret(regs);
                record.head.regs = regs;
                capture_buffer(pid, &record);
                std::cerr << std::format("[{:4}]   = {}\n", index,
                                         render_result(record));
                if (log != nullptr) log->append(record);
                ++index;
            }
            at_entry = !at_entry;
            signal = 0;
        }

        // Anything else is forwarded untouched. Swallowing a signal makes an
        // asynchronous one vanish and loops a faulting instruction forever.
        if (!go(signal)) {
            const char* why = errno == ESRCH ? "tracee gone, or not stopped"
                                             : std::strerror(errno);
            return abandon(pid, std::format("ptrace resume failed: {}", why));
        }
    }
}

}  // namespace retrace
