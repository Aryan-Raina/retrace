# retrace — overview

A record–replay debugger for single-threaded x86-64 Linux programs, built on
`ptrace(2)`. It records everything the kernel tells a program, then replays the
program by feeding those answers back instead of asking the kernel again.

Read the numbered documents in order; each covers one feature.

| Doc | Feature |
|---|---|
| [01-tracing](01-tracing.md) | Trace every system call at entry and exit |
| [02-recording](02-recording.md) | Write outputs, memory buffers and registers to a versioned binary log |
| [03-replay](03-replay.md) | Inject recorded results, detect divergence |
| [04-breakpoints](04-breakpoints.md) | Software breakpoints and reverse-continue |

## The one idea

A program is deterministic apart from what the outside world tells it. Same
binary, same instructions — the only things that can differ between two runs are
the answers the kernel gives back: what a file contained, what time it is, what
`getrandom` produced.

Every one of those answers arrives through a **system call**. So if you can see
and control the syscall boundary, you can see and control everything that makes
a program non-deterministic. That is the whole project.

## The demo

```sh
cmake -S . -B build && cmake --build build

# 1. Trace
printf 'hello from the file\n' > /tmp/in.txt
./build/retrace record -- ./build/fixtures/reader /tmp/in.txt

# 2. Record to a log
./build/retrace record -o /tmp/s.log -- ./build/fixtures/reader /tmp/in.txt
./build/retrace show /tmp/s.log
strings /tmp/s.log | grep hello          # the file's bytes are in the log

# 3. Replay: change the file, the program still sees the old contents
printf 'COMPLETELY DIFFERENT\n' > /tmp/in.txt
./build/fixtures/reader /tmp/in.txt                      # reads the new text
./build/retrace replay /tmp/s.log -- ./build/fixtures/reader /tmp/in.txt
                                                          # reads the old text

# 4. Divergence: same log, different program
./build/retrace replay /tmp/s.log -- ./build/fixtures/echo_args hi
# retrace: divergence at syscall 29: recorded openat, got fstat   (exit 124)

# 5. Breakpoints and reverse-continue
nm build/fixtures/counter | grep ' work'                  # 0000000000401136 T work
./build/retrace record -o /tmp/c.log -- ./build/fixtures/counter
./build/retrace replay --break 0x401136 /tmp/c.log -- ./build/fixtures/counter
# c, c, rc, r, q
```

## How the pieces fit

```
main.cpp        parse the command line, dispatch, map outcomes to exit codes
   |
process.cpp     fork, PTRACE_TRACEME, the SIGSTOP handshake, execv,
   |            the errno pipe, and every raw ptrace call
   |
   +-- trace.cpp        run and record: the wait loop, PTRACE_SYSCALL,
   |                    entry/exit pairing, buffer capture
   |
   +-- replay.cpp       replay: divergence check, cancel-and-inject,
   |                    breakpoint hits, the prompt
   |
   +-- log.cpp          the binary format, reading, writing, rendering
   +-- breakpoint.cpp   0xCC patching, step-over
   +-- syscalls.cpp     number -> name, generated from asm/unistd_64.h
```

## The launch handshake

Every mode starts the same way:

```
retrace                          target
-------                          ------
fork ------------------------->  PTRACE_TRACEME
waitpid blocks                   raise(SIGSTOP)
  <-- stop 1: SIGSTOP ---------  frozen
PTRACE_SETOPTIONS
  (EXITKILL | TRACESYSGOOD)      personality(ADDR_NO_RANDOMIZE)
PTRACE_CONT ------------------>  execv(program)
  <-- stop 2: SIGTRAP ---------  frozen at the entry point, nothing run yet
```

The child stops itself before `exec` so the parent has a point where the child
provably exists, is traced, and has not yet run the target — the only moment
where ptrace options can be set with that guarantee. `launch()` then returns
with the target *still stopped*, and each mode decides how to restart it:
`PTRACE_CONT` for `run`, `PTRACE_SYSCALL` for record and replay, and for
breakpoints it is the one moment where code can be patched before a single
instruction has executed.

## What it does not do

Stated plainly, because every one of these is a deliberate decision rather than
an oversight:

- **Single-threaded targets only.** No `clone`/`fork` following.
- **Replay is not total.** `mmap`, `brk`, `write` and `openat` run for real;
  only observation syscalls are injected. See [03-replay](03-replay.md) for why
  injecting `openat` breaks the dynamic loader.
- **Replay needs the same environment.** Different stdin/stdout kinds change
  what glibc does — recording through a pipe and replaying to a file genuinely
  diverges, because glibc probes with `ioctl(TCGETS)` and buffers differently.
- **Breakpoints take runtime addresses**, not symbol names.
- **`PATH` is never searched.** A recording has to name its target exactly.
- **Reverse-continue is O(program length)** — it replays from the start.
  `rr` adds periodic checkpoints to avoid that.

## Exit codes

| | |
|---|---|
| target's own status | passed through |
| target killed by signal *S* | `128 + S` |
| **divergence during replay** | **124** |
| retrace internal error or bad usage | 125 |
| program found but not executable | 126 |
| program not found | 127 |
