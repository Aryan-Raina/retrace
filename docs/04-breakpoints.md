# 4. Software breakpoints and reverse-continue

## The demo

```sh
nm build/fixtures/counter | grep ' work'      # 0000000000401136 T work

retrace record -o c.log -- ./build/fixtures/counter
retrace replay --break 0x401136 c.log -- ./build/fixtures/counter
```

```
stopped at 0x401136, hit 1
breakpoint hit 1 > c
work 0
stopped at 0x401136, hit 2
breakpoint hit 2 > c
work 1
stopped at 0x401136, hit 3
breakpoint hit 3 > rc          <-- go backwards
stopped at 0x401136, hit 2
breakpoint hit 2 >
```

`c` continue, `rc` reverse-continue, `r` registers, `q` quit.

## What a software breakpoint is

One byte.

`0xCC` is the x86 encoding of `INT3`, a one-byte instruction whose only job is
to raise a trap. To set a breakpoint you overwrite the first byte of the target
instruction with `0xCC` and keep the byte you displaced:

```cpp
read_memory(pid, address, &original_, 1);   // save
write_memory(pid, address, &kInt3, 1);      // 0xCC
```

When execution reaches that address the CPU traps, the kernel turns it into
`SIGTRAP`, and because the process is traced, retrace is woken instead.

It is called a *software* breakpoint because it modifies the program's code.
Hardware breakpoints use the CPU's debug registers instead and do not need the
code changed — but x86-64 gives you only four of them.

## Why `rip - 1`

`INT3` is one byte, and the CPU has already executed it by the time the trap is
delivered. So `rip` points one byte *past* the breakpoint:

```cpp
bool is_hit(const Registers& regs) const { return regs.rip == address_ + 1; }
```

The first thing to do on a hit is wind `rip` back, otherwise the tracee would
resume one byte into the real instruction — in the middle of an opcode — and
execute garbage. Winding back immediately also means anything that inspects the
registers sees the address it stopped *at*:

```
breakpoint hit 1 > r
  rip 0x000000401136          <-- the breakpoint, not 0x401137
```

## The step-over dance

Resuming from a breakpoint is not just "continue", because the `0xCC` is still
sitting there — the tracee would trap on it again immediately and never make
progress. The sequence is:

```
1. rewind rip to the breakpoint address
2. put the original byte back                    (disarm)
3. PTRACE_SINGLESTEP, and wait for the stop      (execute exactly that one instruction)
4. write 0xCC back                               (re-arm)
5. resume
```

Step 3 is why single-stepping exists. You need to run exactly one instruction —
the real one — with the breakpoint absent, then restore it before anything else
can reach that address.

## Telling a breakpoint apart from a syscall stop

Both arrive as `WIFSTOPPED` with `SIGTRAP`. This is the ambiguity that
`PTRACE_O_TRACESYSGOOD` was set for back in stage 1: syscall stops come through
as `SIGTRAP | 0x80`, so a plain `SIGTRAP` is a breakpoint.

```cpp
if (signal == SIGTRAP && request.breakpoint != 0) { ... breakpoint ... }
if (signal == (SIGTRAP | 0x80))                  { ... syscall stop ... }
```

Without that option the two would be indistinguishable and this feature could
not coexist with syscall tracing at all.

## Reverse-continue

A process cannot run backwards. CPUs do not have a reverse gear, and the
previous values of registers and memory are simply gone.

So reverse-continue does not rewind anything — it **replays from the start and
stops earlier**:

```
at hit 3, user types rc
  → kill the tracee
  → launch a fresh one
  → replay the log again
  → stop at hit 2
```

This only works because replay is deterministic. The program takes exactly the
same path, because every answer from the outside world comes from the log
rather than the world. Hit 2 in the second run is the same hit 2 as in the
first.

The whole implementation is the loop in `main`:

```cpp
ReplayRequest request{break_at, 1};
for (;;) {
    Launch target = launch(&argv[first]);
    ReplayOutcome outcome = replay(target.pid, records, request);
    if (!outcome.restart) return outcome.code;
    request.stop_at = outcome.stop_at;     // hits - 1
}
```

That is the payoff for stages 1–3. Reverse execution is nearly free once
deterministic replay exists; without it, it is impossible.

It is also honest about its cost: going back one hit re-runs the program from
the beginning. `rr` does the same thing but takes periodic checkpoints so it
only has to replay from the nearest one.

## Why the fixture is built `-no-pie`

`--break` takes a runtime address. For a position-independent executable the
address `nm` reports is a file offset, and the runtime address is that plus
wherever the loader put the image (`0x555555554000` with ASLR off). Building
`counter` with `-no-pie` makes the two the same number, so `nm` output can be
pasted straight into `--break`.

Resolving `--break main` for PIE binaries means parsing `.symtab` and adding
the load base from `/proc/pid/maps` — a separate piece of work with nothing to
do with ptrace.

## Code

| File | Role |
|---|---|
| `src/breakpoint.{hpp,cpp}` | arm, disarm, hit test, rewind, step-over |
| `src/replay.cpp` | the hit branch, hit counting, the prompt |
| `src/main.cpp` | `--break` parsing and the relaunch loop |
