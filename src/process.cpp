// Launching the target and the low-level ptrace calls that drive it.

#include "retrace.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <iostream>

#include <csignal>
#include <fcntl.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

namespace retrace {
namespace {

enum Step : int { kStepTraceme = 1, kStepExec = 2 };

struct ChildFailure {
    int step;
    int error;
};

// ptrace's `data` argument is a void*, so integers must be widened to it.
void* as_data(int value) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(value));
}

// Runs between fork and exec, so it must be async-signal-safe: raw write(2)
// only. Any lock another thread held at fork time stays locked forever.
void report(int fd, int step, int error) {
    const ChildFailure failure{step, error};
    const char* bytes = reinterpret_cast<const char*>(&failure);
    size_t left = sizeof(failure);
    while (left > 0) {
        const ssize_t n = write(fd, bytes, left);
        if (n == -1) {
            if (errno == EINTR) continue;
            return;
        }
        bytes += n;
        left -= static_cast<size_t>(n);
    }
}

ssize_t read_fully(int fd, void* buffer, size_t length) {
    char* out = static_cast<char*>(buffer);
    size_t total = 0;
    while (total < length) {
        const ssize_t n = read(fd, out + total, length - total);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

Launch fail(const char* call, int error) {
    return Launch{.internal_errno = error, .failed_call = call};
}

}  // namespace

void complain(std::string_view message) {
    std::cerr << "retrace: " << message << '\n';
}

bool read_memory(pid_t pid, unsigned long addr, void* out, size_t length) {
    char* dst = static_cast<char*>(out);
    size_t done = 0;
    while (done < length) {
        errno = 0;
        const long word = ptrace(PTRACE_PEEKDATA, pid, addr + done, nullptr);
        if (word == -1 && errno != 0) return false;
        const size_t n = std::min(sizeof(word), length - done);
        std::memcpy(dst + done, &word, n);
        done += n;
    }
    return true;
}

bool write_memory(pid_t pid, unsigned long addr, const void* in, size_t length) {
    const char* src = static_cast<const char*>(in);
    size_t done = 0;
    while (done < length) {
        const size_t n = std::min(sizeof(long), length - done);
        long word = 0;
        if (n < sizeof(long)) {
            // Partial word: read what is there so the bytes we are not
            // replacing survive.
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, addr + done, nullptr);
            if (word == -1 && errno != 0) return false;
        }
        std::memcpy(&word, src + done, n);
        if (ptrace(PTRACE_POKEDATA, pid, addr + done, word) == -1) return false;
        done += n;
    }
    return true;
}

bool wait_for(pid_t pid, int* status) {
    while (waitpid(pid, status, 0) == -1) {
        if (errno != EINTR) return false;
    }
    return true;
}

bool resume(pid_t pid, int signal) {
    return ptrace(PTRACE_CONT, pid, nullptr, as_data(signal)) != -1;
}

bool resume_to_syscall(pid_t pid, int signal) {
    return ptrace(PTRACE_SYSCALL, pid, nullptr, as_data(signal)) != -1;
}

bool single_step(pid_t pid, int signal) {
    return ptrace(PTRACE_SINGLESTEP, pid, nullptr, as_data(signal)) != -1;
}

void kill_and_reap(pid_t pid) {
    const int saved = errno;
    kill(pid, SIGKILL);
    int status = 0;
    (void)wait_for(pid, &status);
    errno = saved;
}

bool get_regs(pid_t pid, Registers* regs) {
    return ptrace(PTRACE_GETREGS, pid, nullptr, regs) != -1;
}

bool set_regs(pid_t pid, const Registers& regs) {
    return ptrace(PTRACE_SETREGS, pid, nullptr, const_cast<Registers*>(&regs)) != -1;
}

// The kernel overwrites rax with -ENOSYS on entry and the result on exit, so
// the number the program asked for survives only in orig_rax.
long syscall_nr(const Registers& regs) { return static_cast<long>(regs.orig_rax); }
long syscall_ret(const Registers& regs) { return static_cast<long>(regs.rax); }

void set_syscall_nr(Registers* regs, long nr) {
    regs->orig_rax = static_cast<unsigned long long>(nr);
}

void set_syscall_ret(Registers* regs, long value) {
    regs->rax = static_cast<unsigned long long>(value);
}

// r10, not rcx: the SYSCALL instruction clobbers rcx with the return address,
// so the kernel ABI differs from the C one in its fourth argument.
long syscall_arg(const Registers& regs, int index) {
    switch (index) {
        case 0: return static_cast<long>(regs.rdi);
        case 1: return static_cast<long>(regs.rsi);
        case 2: return static_cast<long>(regs.rdx);
        case 3: return static_cast<long>(regs.r10);
        case 4: return static_cast<long>(regs.r8);
        case 5: return static_cast<long>(regs.r9);
        default: return 0;
    }
}

Launch launch(char* const* argv) {
    // O_CLOEXEC is the trick: a successful exec closes the write end for us, so
    // the parent reads a payload on failure and EOF on success.
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) == -1) return fail("pipe2", errno);

    const pid_t pid = fork();
    if (pid == -1) {
        const int saved = errno;
        close(fds[0]);
        close(fds[1]);
        return fail("fork", saved);
    }

    if (pid == 0) {
        close(fds[0]);
        if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
            report(fds[1], kStepTraceme, errno);
            _exit(127);
        }
        // Stopping here gives the parent a point where the child provably
        // exists, is traced, and has not yet run the target.
        raise(SIGSTOP);
        // Replay compares runs and breakpoints are absolute, so the load base
        // has to be identical every time.
        (void)personality(ADDR_NO_RANDOMIZE);
        // execv, never execvp: execvp re-runs an unrecognised file under
        // /bin/sh, which would leave retrace tracing a shell.
        execv(argv[0], argv);
        report(fds[1], kStepExec, errno);
        _exit(127);
    }

    close(fds[1]);

    // The pipe cannot be read first: the child stops before exec, so nothing
    // would arrive and the read would block forever.
    int status = 0;
    if (!wait_for(pid, &status)) {
        const int saved = errno;
        close(fds[0]);
        return fail("waitpid", saved);
    }

    ChildFailure failure{};
    auto from_child = [&] {
        return failure.step == kStepTraceme
                   ? fail("ptrace(PTRACE_TRACEME)", failure.error)
                   : Launch{.exec_errno = failure.error};
    };

    if (!WIFSTOPPED(status)) {
        // Died instead of stopping, so PTRACE_TRACEME failed. Already reaped.
        const ssize_t n = read_fully(fds[0], &failure, sizeof(failure));
        close(fds[0]);
        return n == static_cast<ssize_t>(sizeof(failure)) ? from_child()
                                                          : fail("fork", ECHILD);
    }

    auto abort_launch = [&](const char* call) {
        const int saved = errno;
        close(fds[0]);
        kill_and_reap(pid);
        return fail(call, saved);
    };

    if (WSTOPSIG(status) != SIGSTOP) {
        errno = EPROTO;
        return abort_launch("handshake");
    }

    // Options can only be set at a ptrace-stop, and EXITKILL has to be in force
    // before the target runs an instruction. TRACESYSGOOD marks syscall stops
    // with bit 7 so they are distinguishable from a real SIGTRAP.
    if (ptrace(PTRACE_SETOPTIONS, pid, nullptr,
               as_data(PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD)) == -1) {
        return abort_launch("ptrace(PTRACE_SETOPTIONS)");
    }

    if (!resume(pid, 0)) return abort_launch("ptrace(PTRACE_CONT)");

    const ssize_t n = read_fully(fds[0], &failure, sizeof(failure));
    close(fds[0]);
    if (n == static_cast<ssize_t>(sizeof(failure))) {
        kill_and_reap(pid);
        return from_child();
    }

    // EOF means exec worked. The kernel raises SIGTRAP on a successful execve
    // in a traced process, before any instruction of the new image runs.
    if (!wait_for(pid, &status)) {
        const int saved = errno;
        kill_and_reap(pid);
        return fail("waitpid", saved);
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
        kill_and_reap(pid);
        errno = EPROTO;
        return fail("handshake", EPROTO);
    }

    // Left stopped: the caller decides how to restart it.
    return Launch{.pid = pid};
}

}  // namespace retrace
