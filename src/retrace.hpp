#pragma once

#include <cstddef>
#include <string_view>
#include <sys/types.h>
#include <sys/user.h>

namespace retrace {

// Everything retrace says goes to stderr; stdout belongs to the target.
void complain(std::string_view message);

struct Launch {
    pid_t pid = -1;           // loaded, traced, stopped at the entry point
    int exec_errno = 0;       // the program could not be run
    int internal_errno = 0;   // retrace itself failed
    const char* failed_call = nullptr;
};

Launch launch(char* const* argv);

using Registers = user_regs_struct;

bool get_regs(pid_t pid, Registers* regs);
bool set_regs(pid_t pid, const Registers& regs);

long syscall_nr(const Registers& regs);              // orig_rax, not rax
long syscall_arg(const Registers& regs, int index);  // rdi rsi rdx r10 r8 r9
long syscall_ret(const Registers& regs);

// Writing orig_rax at syscall entry changes, or cancels, the call the kernel
// is about to make; writing rax at exit changes what the program sees.
void set_syscall_nr(Registers* regs, long nr);
void set_syscall_ret(Registers* regs, long value);

// PTRACE_PEEKDATA returns -1 on failure, but -1 is also a legitimate word, so
// errno has to be cleared before the call and checked after it.
bool read_memory(pid_t pid, unsigned long addr, void* out, size_t length);
bool write_memory(pid_t pid, unsigned long addr, const void* in, size_t length);

bool wait_for(pid_t pid, int* status);
bool resume(pid_t pid, int signal);
bool resume_to_syscall(pid_t pid, int signal);
bool single_step(pid_t pid, int signal);
void kill_and_reap(pid_t pid);

enum class Mode { kRun, kRecord, kReplay };

class LogWriter;

// Drives the target to completion. Returns retrace's exit code.
int supervise(pid_t pid, Mode mode, LogWriter* log);

}  // namespace retrace
