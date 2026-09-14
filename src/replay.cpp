// Replaying a recorded execution.
//
// At each syscall-entry stop we check the target asked for the same call as
// last time, then cancel the call by setting orig_rax to -1: the kernel sees
// an invalid syscall number and does nothing. At the matching exit stop we
// write the recorded return value into rax and copy the recorded bytes back
// into the target's buffer, so the program sees the answer it saw when the log
// was made -- without any real I/O having happened.

#include "replay.hpp"

#include "breakpoint.hpp"

#include "retrace.hpp"
#include "syscalls.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>
#include <istream>
#include <string>

#include <csignal>
#include <sys/syscall.h>
#include <sys/wait.h>

namespace retrace {
namespace {

constexpr int kExitInternal = 125;

// Setting orig_rax to this makes the kernel reject the call as invalid, which
// is how a syscall is cancelled without the program noticing.
constexpr long kCancelled = -1;

// Syscalls whose result is an observation of the outside world, and which can
// therefore be answered from the log.
//
// Everything else runs for real, and the omissions are deliberate. mmap, brk
// and mprotect build the address space, so faking them leaves the program with
// nowhere to run. write has to happen or the replay would be silent. And
// openat is excluded because a fabricated file descriptor would then be handed
// to a real mmap by the dynamic loader, which fails with EBADF before main()
// is ever reached.
bool should_inject(long nr) {
    switch (nr) {
        case SYS_read:
        case SYS_pread64:
        case SYS_readv:
        case SYS_getrandom:
        case SYS_getdents64:
        case SYS_readlink:
        case SYS_uname:
        case SYS_clock_gettime:
        case SYS_gettimeofday:
        case SYS_time:
        case SYS_getpid:
        case SYS_getuid:
        case SYS_geteuid:
            return true;
        default:
            return false;
    }
}

std::string name_of(long nr) {
    const char* name = syscall_name(nr);
    return name != nullptr ? name : std::format("syscall_{}", nr);
}

ReplayOutcome abandon(pid_t pid, const std::string& message) {
    kill_and_reap(pid);
    complain(message);
    return ReplayOutcome{.code = kExitInternal};
}

ReplayOutcome diverged(pid_t pid, const std::string& message) {
    kill_and_reap(pid);
    complain(message);
    return ReplayOutcome{.code = kExitDiverged};
}

void print_registers(const Registers& regs) {
    std::cerr << std::format(
        "  rip 0x{:012x}  rsp 0x{:012x}  rbp 0x{:012x}\n"
        "  rax 0x{:012x}  rbx 0x{:012x}  rcx 0x{:012x}\n"
        "  rdx 0x{:012x}  rsi 0x{:012x}  rdi 0x{:012x}\n",
        regs.rip, regs.rsp, regs.rbp, regs.rax, regs.rbx, regs.rcx,
        regs.rdx, regs.rsi, regs.rdi);
}

enum class Choice { kContinue, kReverse, kQuit };

// Reads one command from stdin. The target inherits stdin too, so this is only
// usable with a target that does not read from it.
Choice prompt(pid_t pid, unsigned long hit) {
    for (;;) {
        std::cerr << std::format("breakpoint hit {} > ", hit);
        std::string line;
        if (!std::getline(std::cin, line)) return Choice::kQuit;

        if (line == "c" || line == "continue") return Choice::kContinue;
        if (line == "rc" || line == "reverse-continue") {
            if (hit <= 1) {
                std::cerr << "already at the first hit\n";
                continue;
            }
            return Choice::kReverse;
        }
        if (line == "q" || line == "quit") return Choice::kQuit;
        if (line == "r" || line == "regs") {
            Registers regs{};
            if (get_regs(pid, &regs)) print_registers(regs);
            continue;
        }
        std::cerr << "  c = continue, rc = reverse-continue, r = regs, q = quit\n";
    }
}

}  // namespace

ReplayOutcome replay(pid_t pid, const std::vector<Record>& records,
                     const ReplayRequest& request) {
    Breakpoint breakpoint;
    if (request.breakpoint != 0 && !breakpoint.arm(pid, request.breakpoint)) {
        return abandon(pid, std::format("could not set a breakpoint at 0x{:x}",
                                        request.breakpoint));
    }
    unsigned long hits = 0;
    unsigned long stop_at = request.stop_at;

    if (!resume_to_syscall(pid, 0)) {
        return abandon(pid, std::format("ptrace resume failed: {}", std::strerror(errno)));
    }

    bool at_entry = true;
    bool cancelled = false;
    size_t index = 0;

    for (;;) {
        int status = 0;
        if (!wait_for(pid, &status)) {
            return abandon(pid, std::format("waitpid failed: {}", std::strerror(errno)));
        }

        if (WIFEXITED(status)) {
            if (index < records.size()) {
                complain(std::format(
                    "target exited after {} of {} recorded syscalls", index,
                    records.size()));
                return ReplayOutcome{.code = kExitDiverged};
            }
            return ReplayOutcome{.code = WEXITSTATUS(status)};
        }
        if (WIFSIGNALED(status)) {
            const int signal = WTERMSIG(status);
            complain(std::format("terminated by signal {}", signal));
            return ReplayOutcome{.code = 128 + signal};
        }
        if (!WIFSTOPPED(status)) {
            return abandon(pid, "unexpected wait status");
        }

        int signal = WSTOPSIG(status);

        if (signal == SIGTRAP && request.breakpoint != 0) {
            Registers regs{};
            if (!get_regs(pid, &regs)) {
                return abandon(pid, "ptrace(PTRACE_GETREGS) failed");
            }
            if (breakpoint.is_hit(regs)) {
                if (!breakpoint.rewind(pid, &regs)) {
                    return abandon(pid, "could not rewind rip after a breakpoint");
                }
                ++hits;
                if (hits >= stop_at) {
                    std::cerr << std::format("\nstopped at 0x{:x}, hit {}\n",
                                             breakpoint.address(), hits);
                    const Choice choice = prompt(pid, hits);
                    if (choice == Choice::kQuit) {
                        kill_and_reap(pid);
                        return ReplayOutcome{.code = 0};
                    }
                    if (choice == Choice::kReverse) {
                        kill_and_reap(pid);
                        return ReplayOutcome{
                            .code = 0, .restart = true, .stop_at = hits - 1};
                    }
                    stop_at = hits + 1;
                }
                if (!breakpoint.step_over(pid)) {
                    return abandon(pid, "could not step over the breakpoint");
                }
                if (!resume_to_syscall(pid, 0)) {
                    return abandon(pid, "ptrace resume failed");
                }
                continue;
            }
            signal = 0;
        }

        if (signal == (SIGTRAP | 0x80)) {
            Registers regs{};
            if (!get_regs(pid, &regs)) {
                return abandon(pid, std::format("ptrace(PTRACE_GETREGS) failed: {}",
                                                std::strerror(errno)));
            }

            if (at_entry) {
                const long actual_nr = syscall_nr(regs);
                if (index >= records.size()) {
                    // exit_group never returns, so it never reaches an exit
                    // stop and is never recorded. Running off the end of the
                    // log with one is the normal way a replay finishes.
                    if (actual_nr == SYS_exit_group || actual_nr == SYS_exit) {
                        if (!resume_to_syscall(pid, 0)) {
                            return abandon(pid, "ptrace resume failed");
                        }
                        continue;
                    }
                    return diverged(
                        pid, std::format("divergence at syscall {}: log ended, but "
                                         "the target called {}",
                                         index, name_of(actual_nr)));
                }

                const Record& expected = records[index];
                const long actual = actual_nr;
                if (actual != expected.head.nr) {
                    return diverged(
                        pid,
                        std::format("divergence at syscall {}: recorded {}, got {}",
                                    index, name_of(expected.head.nr), name_of(actual)));
                }

                cancelled = should_inject(actual);
                if (cancelled) {
                    Registers patched = regs;
                    set_syscall_nr(&patched, kCancelled);
                    if (!set_regs(pid, patched)) {
                        return abandon(pid,
                                       std::format("ptrace(PTRACE_SETREGS) failed: {}",
                                                   std::strerror(errno)));
                    }
                }
            } else {
                if (cancelled) {
                    const Record& expected = records[index];

                    // The kernel left -ENOSYS in rax because we cancelled the
                    // call; overwrite it with what the program saw last time.
                    Registers patched = regs;
                    set_syscall_ret(&patched, expected.head.ret);
                    if (!set_regs(pid, patched)) {
                        return abandon(pid,
                                       std::format("ptrace(PTRACE_SETREGS) failed: {}",
                                                   std::strerror(errno)));
                    }

                    // Put the recorded bytes back where the kernel would have
                    // written them. The buffer address comes from this run's
                    // registers, not the log: the program may have placed it
                    // somewhere else.
                    if (expected.head.buf_len > 0 && expected.head.buf_arg >= 0) {
                        const auto address = static_cast<unsigned long>(
                            syscall_arg(regs, expected.head.buf_arg));
                        if (!write_memory(pid, address, expected.buf.data(),
                                          expected.head.buf_len)) {
                            return abandon(pid, "could not write the recorded buffer "
                                                "back into the target");
                        }
                    }
                }
                ++index;
            }
            at_entry = !at_entry;
            signal = 0;
        }

        if (!resume_to_syscall(pid, signal)) {
            return abandon(pid, std::format("ptrace resume failed: {}",
                                            std::strerror(errno)));
        }
    }
}

}  // namespace retrace
