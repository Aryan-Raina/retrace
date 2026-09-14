# 1. Tracing system calls at entry and exit

## What this does

```sh
retrace record -- ./build/fixtures/reader /tmp/in.txt
```

```
[  29] openat(0xffffffffffffff9c, 0x7fffffffe699, 0x0, 0x0, 0x0, 0x0)
[  29]   = 3 (0x3)
[  30] read(0x3, 0x7fffffffe0b0, 0xff, 0x0, 0x0, 0x0)
[  30]   = 20 (0x14)
[  31] close(0x3, 0x0, 0x0, 0x0, 0x0, 0x0)
[  31]   = 0 (0x0)
[  35] write(0x1, 0x555555559010, 0x23, 0x0, 0x0, 0x0)
read 20 bytes: hello from the file
[  35]   = 35 (0x23)
```

A cut-down `strace`. Two lines per syscall: the request, then what it returned.

## The idea

A process cannot do anything interesting on its own. Open a file, read from it,
print something, allocate memory — every one of those is a request to the
kernel. So **the syscall boundary is where a program meets the outside world**,
and if you can see and control that boundary you can see and control everything
that makes a program non-deterministic.

That is why the whole project hangs off this one loop.

## The mechanism

The launcher already had a wait loop that resumed the tracee with `PTRACE_CONT`
("run until something happens"). Tracing syscalls is one substitution:

```c
PTRACE_CONT     →  run until the next signal
PTRACE_SYSCALL  →  run until the next syscall boundary, then stop
```

With `PTRACE_SYSCALL`, the kernel freezes the tracee **twice** per syscall:

1. **Entry stop** — arguments are in the registers, the call has *not* run yet.
   This is where replay later gets to change or cancel it.
2. **Exit stop** — the call has run, and the return value is in `rax`.

Everything else in the loop — the `waitpid`, the signal forwarding, the
terminal-status handling — is unchanged.

## Three things that will bite you

### 1. The syscall number is in `orig_rax`, not `rax`

This is the classic mistake. On x86-64 a program puts the syscall number in
`rax` and executes the `syscall` instruction. But the kernel then *overwrites*
`rax`: with `-ENOSYS` at entry, and with the return value at exit.

So the kernel keeps a copy of what the program originally asked for in a
separate slot, `orig_rax`. Read `rax` at a syscall stop and you get `-38`
(`-ENOSYS`) for every single call.

```cpp
long syscall_number(const Registers& regs) { return regs.orig_rax; }  // right
long syscall_return(const Registers& regs) { return regs.rax; }       // exit only
```

### 2. Arguments use `r10`, not `rcx`

The ordinary x86-64 C calling convention passes arguments in
`rdi, rsi, rdx, rcx, r8, r9`. The **kernel** convention is
`rdi, rsi, rdx, r10, r8, r9` — the fourth register differs.

The reason is hardware: the `syscall` instruction itself clobbers `rcx` (it
stashes the return address there), so `rcx` cannot survive to carry an
argument. Linux uses `r10` instead.

### 3. The kernel does not tell you whether a stop is entry or exit

Both are the same event. `waitpid` reports them identically. There is no flag
to check.

The standard answer is that they strictly alternate, so you track a boolean:

```cpp
bool at_entry = true;
// ...on each syscall stop:
at_entry = !at_entry;
```

The reason this is safe *here* is that `launch()` hands the tracee back stopped
at its entry point, before any syscall of the program has begun — so the first
syscall stop we ever see is guaranteed to be an entry. If you attached to a
process already running, you would have no such guarantee, and you would need
`PTRACE_GET_SYSCALL_INFO` (Linux 5.3+), which reports the stop type explicitly.

## Telling a syscall stop from a real SIGTRAP

Both arrive as `WIFSTOPPED` with `SIGTRAP`. That is a genuine ambiguity, and it
matters later when breakpoints exist and generate real `SIGTRAP`s.

`PTRACE_O_TRACESYSGOOD` fixes it: with that option set, the kernel reports
syscall stops as `SIGTRAP | 0x80` instead of plain `SIGTRAP`.

```cpp
bool is_syscall_stop(int status) { return WSTOPSIG(status) == (SIGTRAP | 0x80); }
```

The option is set once, in `launch()`, alongside `PTRACE_O_EXITKILL`.

## ASLR is now off

`launch()` calls `personality(ADDR_NO_RANDOMIZE)` in the child before `exec`.
You can see it in the trace: addresses come out as `0x555555...` (the program)
and `0x7ffff7...` (libraries), the same numbers on every run.

Two later features depend on this:

- **Replay** compares one run against another; if addresses moved between runs
  the comparison would be noise.
- **Breakpoints** are given as absolute addresses, which have to mean the same
  thing every time.

It is one line, it happens in the child, and it survives `exec`.

## Why the trace starts at syscall ~22

The first twenty-odd syscalls are the dynamic loader: `mmap`ing `ld.so`,
`openat`ing `libc.so.6`, `mprotect`ing segments. Your `main` has not run yet.
`execve` itself never appears, because the tracee was still stopped at the exec
trap when we started asking for syscall stops.

## Code

| File | Role |
|---|---|
| `src/recorder.cpp` | the loop: `PTRACE_SYSCALL`, entry/exit tracking, rendering |
| `src/registers.{hpp,cpp}` | `PTRACE_GETREGS`/`SETREGS`, `orig_rax` and ABI argument accessors |
| `src/syscall_table.cpp` | number → name, generated once from `asm/unistd_64.h` |
| `src/tracee.cpp` | `resume_to_syscall()` |
| `src/launcher.cpp` | `TRACESYSGOOD`, `ADDR_NO_RANDOMIZE`, leaves the tracee stopped at entry |

## Demo

```sh
cmake --build build
printf 'hello from the file\n' > /tmp/in.txt
./build/retrace record -- ./build/fixtures/reader /tmp/in.txt
```

Compare against the real thing:

```sh
strace -f ./build/fixtures/reader /tmp/in.txt
```
