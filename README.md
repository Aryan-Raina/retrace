# retrace

A **record–replay debugger** for single-threaded x86-64 Linux programs, built
directly on `ptrace(2)`.

It records everything the kernel tells a program — return values, the bytes
written into buffers, the register state — into a versioned binary log. It can
then replay the program by feeding those answers back instead of asking the
kernel again, report where a run stops matching the recording, and stop at
software breakpoints that you can step *backwards* through.

```sh
retrace record -o session.log -- ./myprogram input.txt
retrace show   session.log
retrace replay session.log -- ./myprogram input.txt
retrace replay --break 0x401136 session.log -- ./myprogram input.txt
```

## What it does

| | |
|---|---|
| **Trace** | every system call at entry and exit, with names and arguments |
| **Record** | return values, memory buffers and full register state to a versioned binary log |
| **Replay** | inject the recorded results so the program never touches the real world |
| **Diverge** | detect and report where a run stops matching the recording |
| **Break** | software breakpoints via `0xCC`, with `continue` and `reverse-continue` |

## The idea

A program is deterministic apart from what the outside world tells it. Same
binary, same instructions — the only things that can differ between two runs are
the kernel's answers: what a file contained, what time it is, what `getrandom`
returned.

All of those arrive through system calls. Control the syscall boundary and you
control everything non-deterministic about the program.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

C++20 compiler, CMake ≥ 3.20, Linux x86-64. No third-party dependencies.

```sh
cmake --install build --prefix ~/.local     # puts retrace on your PATH
```

## The demo

```sh
printf 'hello from the file\n' > /tmp/in.txt

# Record
./build/retrace record -o /tmp/s.log -- ./build/fixtures/reader /tmp/in.txt
#   read 20 bytes: hello from the file

# The file's actual bytes are in the log
strings /tmp/s.log | grep hello

# Change the file completely
printf 'COMPLETELY DIFFERENT\n' > /tmp/in.txt

./build/fixtures/reader /tmp/in.txt
#   read 21 bytes: COMPLETELY DIFFERENT      <- real run, new contents

./build/retrace replay /tmp/s.log -- ./build/fixtures/reader /tmp/in.txt
#   read 20 bytes: hello from the file       <- replay, recorded contents
```

Divergence:

```sh
./build/retrace replay /tmp/s.log -- ./build/fixtures/echo_args hi
# retrace: divergence at syscall 29: recorded openat, got fstat
# exit 124
```

Breakpoints and reverse-continue:

```sh
nm build/fixtures/counter | grep ' work'        # 0000000000401136 T work
./build/retrace record -o /tmp/c.log -- ./build/fixtures/counter
./build/retrace replay --break 0x401136 /tmp/c.log -- ./build/fixtures/counter
```

```
stopped at 0x401136, hit 1
breakpoint hit 1 > c
work 0
stopped at 0x401136, hit 2
breakpoint hit 2 > c
work 1
stopped at 0x401136, hit 3
breakpoint hit 3 > rc
stopped at 0x401136, hit 2
breakpoint hit 2 > r
  rip 0x000000401136  rsp 0x7fffffffe228  rbp 0x7fffffffe240
```

`c` continue · `rc` reverse-continue · `r` registers · `q` quit

## Usage

```
retrace run              -- <program-path> [args...]
retrace record [-o LOG]  -- <program-path> [args...]
retrace show   LOG
retrace replay [--break 0xADDR] LOG -- <program-path> [args...]
```

The `--` separator is **required** and everything after it goes to the target
verbatim — no flag parsing, no shell, no expansion.

**`PATH` is not searched.** Name the program by path (`./prog`, `/bin/prog`); a
bare name is rejected. Which binary a name resolves to depends on the
environment, and a recording has to identify exactly what it ran.

## Exit codes

| Situation | Code |
|---|---|
| target exited normally | its own status |
| target killed by signal *S* | `128 + S` |
| **divergence during replay** | **124** |
| retrace internal error or bad usage | 125 |
| program found but not executable | 126 |
| program not found | 127 |

## Documentation

`docs/` explains how and why, one document per feature:

| | |
|---|---|
| [00-overview](docs/00-overview.md) | the system end to end |
| [01-tracing](docs/01-tracing.md) | syscall entry/exit, `orig_rax`, the ABI registers |
| [02-recording](docs/02-recording.md) | the log format, reading tracee memory |
| [03-replay](docs/03-replay.md) | cancel-and-inject, divergence |
| [04-breakpoints](docs/04-breakpoints.md) | `0xCC`, the step-over dance, reverse-continue |

## Limitations

Deliberate, not accidental:

- **Single-threaded targets only.** No `clone`/`fork` following.
- **Replay is not total.** `mmap`, `brk`, `write` and `openat` run for real —
  only observation syscalls are injected. Injecting `openat` would hand a
  fabricated descriptor to the dynamic loader's `mmap`, which fails with
  `EBADF` before `main`.
- **Replay needs the same environment.** glibc probes stdout with
  `ioctl(TCGETS)` and buffers differently for a pipe, a file and a terminal, so
  recording through one and replaying to another genuinely diverges.
- **Breakpoints take runtime addresses**, not symbol names. Build with
  `-no-pie` and `nm` gives you the address directly.
- **Reverse-continue is O(program length)** — it replays from the start. `rr`
  adds periodic checkpoints.

## Layout

| | |
|---|---|
| `src/main.cpp` | command line, dispatch, exit codes |
| `src/process.cpp` | fork, the trace handshake, `execv`, the errno pipe, raw ptrace |
| `src/trace.cpp` | the wait loop, in `run` and `record` modes |
| `src/replay.cpp` | divergence, injection, breakpoint hits, the prompt |
| `src/log.cpp` | the binary format |
| `src/breakpoint.cpp` | `0xCC` patching and step-over |
| `src/syscalls.cpp` | syscall names, generated from `asm/unistd_64.h` |
| `tests/run_acceptance.sh` | 82 black-box cases |
| `tests/fixtures/` | small C programs that misbehave on purpose |

## Tests

```sh
ctest --test-dir build --output-on-failure
```

Black-box only: exit status, stdout, stderr. The cases written before `ptrace`
was introduced still pass unchanged, which is the evidence that the tracer stays
invisible to the program it traces.
