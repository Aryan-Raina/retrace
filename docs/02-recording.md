# 2. Recording to a versioned binary log

## What this does

```sh
retrace record -o session.log -- ./build/fixtures/reader input.txt
retrace show session.log
```

```
[  29] openat(0xffffffffffffff9c, 0x7fffffffe699, 0x0, 0x0, 0x0, 0x0)
[  29]   = 4 (0x4)
[  30] read(0x4, 0x7fffffffe0b0, 0xff, 0x0, 0x0, 0x0)
[  30]   = 20 (0x14)  [20 bytes captured]
36 records
```

The log is a real file. `strings session.log` finds `hello from the file` in it,
because the bytes `read(2)` returned were copied out of the tracee and stored.

## What "outputs, memory buffers and register states" means

Three different kinds of thing come back from a syscall, and all three have to
be recorded for replay to work later:

| Kind | Where it lives | Example |
|---|---|---|
| **Return value** | `rax` at syscall exit | `read` returned 20 |
| **Memory buffer** | memory the kernel wrote into | the 20 bytes themselves |
| **Register state** | the whole register file | `rip`, `rsp`, all of it |

Missing any one of them breaks replay. The return value alone tells the program
*how much* it read but not *what*. The buffer alone does not tell it the call
succeeded.

## The format

Deliberately boring: a header, then a sequence of records.

```c
struct LogHeader {
    char     magic[8];   // "RETRACE"
    uint32_t version;    // 1
    uint32_t arch;       // 1 = x86-64
    uint64_t count;      // filled in when the writer closes
};

struct RecordHeader {
    int64_t   nr, args[6], ret;
    Registers regs;       // 216 bytes, the full user_regs_struct
    uint32_t  buf_len;    // captured bytes following this header
    int32_t   buf_arg;    // which argument held the buffer, -1 if none
};                        // then buf_len bytes
```

Writing a record is one `fwrite` of the header and one of the bytes. Reading is
the mirror image. There is no serialisation library and nothing to parse.

`count` cannot be known until the program ends, so the writer seeks back to
`offsetof(LogHeader, count)` on close and patches it. Reading then cross-checks
it against the number of records actually found, which catches a truncated log.

## What makes it *versioned*

A version field nobody checks is decoration. `read_log` refuses three things:

```
$ retrace show not-a-log
retrace: not-a-log: not a retrace log            # magic mismatch

$ retrace show from-the-future.log
retrace: log version 9, this build reads version 1

$ retrace show truncated.log
retrace: truncated.log: header says 36 records, found 30
```

That is what lets you say the format is versioned and mean it.

## Capturing the memory buffer

Some syscalls return data by writing into a buffer the caller supplied. For
`read(fd, buf, n)` the interesting bytes are at `buf` and there are `ret` of
them — but `buf` is an address in *the tracee's* address space, not ours.

`read_memory` copies it out with `PTRACE_PEEKDATA`, one word at a time:

```cpp
errno = 0;
const long word = ptrace(PTRACE_PEEKDATA, pid, addr + done, nullptr);
if (word == -1 && errno != 0) return false;
```

**The `errno = 0` is not optional.** `PTRACE_PEEKDATA` returns the word it read,
and returns `-1` on failure — but `-1` is also a perfectly legal word value.
The only way to tell them apart is to clear `errno` first and check it after.

Which syscalls have such a buffer, and in which argument, is a small table:

```cpp
int buffer_arg_of(long nr) {
    switch (nr) {
        case SYS_read: case SYS_pread64: case SYS_getrandom:
        case SYS_getdents64: case SYS_readlink: case SYS_recvfrom:
            return 1;              // f(fd, BUF, n)
        case SYS_uname: return 0;  // uname(BUF)
        default: return -1;
    }
}
```

Six entries, not 350. Enough for the programs this tool is meant to replay, and
adding one is a single line.

## Where it plugs into the loop

Nothing about the wait loop changed. At the syscall-*entry* stop the number and
arguments are saved into a pending record; at the *exit* stop the return value,
the registers and the buffer are filled in and the record is appended. The same
`Record` is what `show` renders, so a log always reads back exactly the way it
was printed live.

## Code

| File | Role |
|---|---|
| `src/log.{hpp,cpp}` | the format, `LogWriter`, `read_log`, the buffer table, rendering |
| `src/trace.cpp` | `capture_buffer`, and filling the record across entry and exit |
| `src/process.cpp` | `read_memory` / `write_memory` via `PEEKDATA` / `POKEDATA` |

## Demo

```sh
printf 'hello from the file\n' > /tmp/in.txt
./build/retrace record -o /tmp/s.log -- ./build/fixtures/reader /tmp/in.txt
./build/retrace show /tmp/s.log | tail
strings /tmp/s.log | grep hello      # the file's bytes really are in there
```
