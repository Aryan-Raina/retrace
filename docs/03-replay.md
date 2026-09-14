# 3. Replay: injecting recorded results, detecting divergence

This is the core of the project. Everything before it was setup.

## The demo

```sh
printf 'hello from the file\n' > in.txt
retrace record -o s.log -- ./reader in.txt
        # read 20 bytes: hello from the file

printf 'THE FILE NOW SAYS SOMETHING ELSE ENTIRELY\n' > in.txt

./reader in.txt
        # read 42 bytes: THE FILE NOW SAYS SOMETHING ELSE ENTIRELY

retrace replay s.log -- ./reader in.txt
        # read 20 bytes: hello from the file      <-- the OLD contents
```

The file on disk says one thing; the program prints another. It is reading from
the log, not from the filesystem.

## The idea

A program is deterministic apart from what the outside world tells it. Same
code, same instructions — the only things that can differ between two runs are
the answers the kernel gives back: what a file contained, what time it is, what
`getrandom` produced.

So if you record every answer, you can replay the program by **giving it those
answers again instead of asking the kernel**. The program cannot tell the
difference, because from inside the process the two are identical.

## Cancelling a syscall

The trick that makes this work is one line:

```cpp
set_syscall_nr(&patched, -1);   // orig_rax = -1
set_regs(pid, patched);
```

At a syscall-entry stop the call has **not happened yet**. If we overwrite
`orig_rax` with `-1`, the kernel looks up syscall number −1, finds nothing, and
returns `-ENOSYS` without doing any work. No file is read, no clock is
consulted, nothing happens.

Then at the exit stop we replace the `-ENOSYS` the kernel left behind:

```cpp
set_syscall_ret(&patched, expected.head.ret);      // rax = 20
write_memory(pid, buffer_address, expected.buf.data(), expected.head.buf_len);
```

`rax` becomes the recorded return value, and the recorded bytes are written
into the buffer the program supplied. From the program's point of view, `read`
returned 20 bytes and there they are.

One detail worth noticing: the buffer address comes from **this run's**
registers, not from the log. The program may have put its buffer somewhere
else this time; what we replay is the *data*, not the address.

## What is injected, and what is not

```cpp
bool should_inject(long nr) {
    switch (nr) {
        case SYS_read: case SYS_pread64: case SYS_readv:
        case SYS_getrandom: case SYS_getdents64: case SYS_readlink:
        case SYS_uname: case SYS_clock_gettime: case SYS_gettimeofday:
        case SYS_time: case SYS_getpid: case SYS_getuid: case SYS_geteuid:
            return true;
        default: return false;   // everything else runs for real
    }
}
```

The omissions are the interesting part, and each has a concrete reason:

| Not injected | Why |
|---|---|
| `mmap`, `brk`, `mprotect` | These *build the address space*. Fake them and the program has nowhere to run |
| `write` | Has to actually happen, or the replay would be silent |
| `exit_group` | The program has to really exit |
| **`openat`** | A fabricated file descriptor gets handed to a real `mmap` by the dynamic loader, which fails with `EBADF` before `main()` is ever reached |

That last one is the constraint that shaped the whole design. It is why the
demo *changes* the file rather than deleting it: `openat` runs for real, so the
file must still exist, but `read` is injected, so its contents are irrelevant.

**This is a limitation, and it is the honest answer to "is your replay total?"
— it is not.** A full record-replay system like `rr` replays `mmap` too, by
mapping from its own copies of the file. That is a much larger system.

## Detecting divergence

At every syscall-entry stop, before anything else:

```cpp
if (actual != expected.head.nr) {
    complain(format("divergence at syscall {}: recorded {}, got {}",
                    index, name_of(expected.head.nr), name_of(actual)));
    return 124;
}
```

```
$ retrace replay s.log -- ./echo_args hello
retrace: divergence at syscall 29: recorded openat, got fstat
$ echo $?
124
```

Note *where* it diverges: syscall 29, not syscall 0. The first ~28 syscalls are
the dynamic loader, which behaves identically for both programs because they
link the same libc. Divergence appears at the first call that belongs to the
program itself.

**Why compare only the syscall number, not the arguments?** Because pointer
arguments legitimately differ between runs — stack addresses shift with the
size of the environment, so comparing them would report divergence constantly
for a program behaving perfectly. What matters is the original system-call
*sequence*, and the sequence is what is checked.

## Two edge cases

**The log runs out.** If the program makes more syscalls than were recorded,
that is divergence — except for `exit_group`, which by definition never returns
and therefore never reaches an exit stop and never gets recorded. Running off
the end of the log with `exit_group` is how a replay normally finishes.

**The program stops early.** If it exits with records left over, it did less
than it did last time. That is also divergence.

## Why ASLR had to go

Replay compares one run against another. If the load base moved, every pointer
argument and every recorded register would be different for reasons that have
nothing to do with program behaviour. `personality(ADDR_NO_RANDOMIZE)` in the
child, set back in stage 1, is what makes runs comparable at all.

## Code

| File | Role |
|---|---|
| `src/replay.cpp` | the loop: divergence check, cancel, inject |
| `src/process.cpp` | `set_syscall_nr`, `set_syscall_ret`, `write_memory` |
| `src/log.cpp` | reading the log back |
