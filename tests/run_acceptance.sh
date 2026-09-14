#!/usr/bin/env bash
#
# Black-box acceptance tests for retrace.
#
#   usage: run_acceptance.sh <path-to-retrace-binary> <fixture-directory>
#
# Every case asserts on the observable contract only: exit status, stdout and
# stderr. Nothing here knows how retrace is implemented, so these tests stay
# valid as ptrace is introduced underneath.

set -uo pipefail

# Resolved to absolute paths: some cases run retrace from a different working
# directory, and a relative path would break there.
RETRACE=$(realpath "${1:?usage: run_acceptance.sh <retrace-binary> <fixture-dir>}")
FIXTURES=$(realpath "${2:?usage: run_acceptance.sh <retrace-binary> <fixture-dir>}")

for required in "$RETRACE" "$FIXTURES/exit_ok" "$FIXTURES/exit_code" \
                "$FIXTURES/crash" "$FIXTURES/echo_args" "$FIXTURES/catch_usr1" \
                "$FIXTURES/reader" "$FIXTURES/counter"; do
    if [ ! -x "$required" ]; then
        printf 'run_acceptance.sh: missing or not executable: %s\n' "$required" >&2
        exit 2
    fi
done

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

passed=0
failed=0

# Runs retrace and captures the three things the contract is defined over.
run() {
    OUT=$("$RETRACE" "$@" 2>"$WORKDIR/stderr")
    RC=$?
    ERR=$(cat "$WORKDIR/stderr")
}

expect_eq() {
    local got=$1 want=$2 what=$3
    if [ "$got" = "$want" ]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        printf '  FAIL %s\n         want: %s\n          got: %s\n' "$what" "$want" "$got"
    fi
}

expect_contains() {
    local got=$1 want=$2 what=$3
    if [[ $got == *"$want"* ]]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        printf '  FAIL %s\n         want substring: %s\n                    got: %s\n' \
               "$what" "$want" "$got"
    fi
}

printf 'acceptance: %s\n' "$RETRACE"

# --- A1: a program that exits successfully ----------------------------------

run run -- "$FIXTURES/exit_ok"
expect_eq "$RC" 0 'A1 exit code is 0'
expect_eq "$OUT" 'hello' 'A1 target stdout passes through untouched'
expect_eq "$ERR" '' 'A1 retrace stays silent on success'

run run -- /bin/true
expect_eq "$RC" 0 'A1 absolute path'

(cd "$FIXTURES" && "$RETRACE" run -- ./exit_ok > /dev/null 2>&1)
expect_eq "$?" 0 'A1 relative path'

# --- A2: a program that exits with a nonzero status -------------------------

for code in 1 42 125 254 255; do
    run run -- "$FIXTURES/exit_code" "$code"
    expect_eq "$RC" "$code" "A2 exit status $code is propagated"
done

run run -- "$FIXTURES/exit_code" 0
expect_eq "$RC" 0 'A2 exit status 0'

# --- A3: a program terminated by a signal -----------------------------------

run run -- "$FIXTURES/crash"
expect_eq "$RC" 134 'A3 SIGABRT gives 128+6'
# No closing paren: whether a core is dumped depends on RLIMIT_CORE and the
# host's core pattern, which the test must not depend on.
expect_contains "$ERR" 'retrace: terminated by signal 6 (SIGABRT' 'A3 SIGABRT reported'

run run -- "$FIXTURES/crash" segv
expect_eq "$RC" 139 'A3 SIGSEGV gives 128+11'
expect_contains "$ERR" 'retrace: terminated by signal 11 (SIGSEGV' 'A3 SIGSEGV reported'

# --- A5: a target that receives a signal and survives it --------------------
# Every other case stops the target exactly once and then it dies, so this is
# the only test that makes the wait loop iterate: stop, forward the signal,
# keep running, wait again. If the tracer dropped the signal instead, the
# target would sit in pause() forever and this would hang rather than fail.

PIDFILE="$WORKDIR/target.pid"
rm -f "$PIDFILE"
"$RETRACE" run -- "$FIXTURES/catch_usr1" "$PIDFILE" > "$WORKDIR/a5.out" 2>"$WORKDIR/a5.err" &
retrace_job=$!

target_pid=""
for _ in $(seq 1 200); do
    if [ -s "$PIDFILE" ]; then
        target_pid=$(cat "$PIDFILE")
        break
    fi
    sleep 0.05
done

if [ -z "$target_pid" ]; then
    kill "$retrace_job" 2>/dev/null
    failed=$((failed + 1))
    printf '  FAIL A5 target never reported its pid\n'
else
    expect_eq "$(awk '/TracerPid/{print $2}' "/proc/$target_pid/status")" "$retrace_job" \
              'A5 the target really is traced by retrace'
    kill -USR1 "$target_pid"
    wait "$retrace_job"
    expect_eq "$?" 0 'A5 target survives a forwarded signal and exits cleanly'
    expect_eq "$(cat "$WORKDIR/a5.out")" 'handler ran' "A5 the target's own handler ran"
    expect_eq "$(cat "$WORKDIR/a5.err")" '' 'A5 retrace stays silent'
fi

# --- A6: the target must not outlive the tracer -----------------------------
# PTRACE_O_EXITKILL is the only cleanup that survives retrace being SIGKILLed,
# so this is the one guarantee no amount of teardown code could provide. The
# target here sits in pause() and would happily run forever on its own.

rm -f "$PIDFILE"
"$RETRACE" run -- "$FIXTURES/catch_usr1" "$PIDFILE" > /dev/null 2>&1 &
retrace_job=$!

target_pid=""
for _ in $(seq 1 200); do
    if [ -s "$PIDFILE" ]; then
        target_pid=$(cat "$PIDFILE")
        break
    fi
    sleep 0.05
done

if [ -z "$target_pid" ]; then
    kill "$retrace_job" 2>/dev/null
    failed=$((failed + 1))
    printf '  FAIL A6 target never reported its pid\n'
else
    kill -KILL "$retrace_job"
    wait "$retrace_job" 2>/dev/null

    survivor="still running"
    for _ in $(seq 1 200); do
        state=$(awk '/^State:/{print $2}' "/proc/$target_pid/status" 2>/dev/null)
        if [ -z "$state" ] || [ "$state" = "Z" ]; then
            survivor="gone"
            break
        fi
        sleep 0.05
    done
    expect_eq "$survivor" 'gone' 'A6 target dies when the tracer is SIGKILLed'
fi

# --- A4: a nonexistent or unusable executable -------------------------------

run run -- "$WORKDIR/no-such-program"
expect_eq "$RC" 127 'A4 missing program gives 127'
expect_eq "$ERR" "retrace: $WORKDIR/no-such-program: No such file or directory" \
          'A4 diagnostic is exact, and printed once by the parent only'
expect_eq "$OUT" '' 'A4 nothing on stdout'

run run -- "$WORKDIR/nope/deeper/still-nothing"
expect_eq "$RC" 127 'A4 missing directory component gives 127'

# A bare name is rejected outright: retrace never searches PATH, so it is a
# usage error rather than a lookup that failed.
run run -- true
expect_eq "$RC" 125 'A4 bare name is a usage error, not a PATH lookup'
expect_contains "$ERR" "retrace: 'true' is not a path" 'A4 bare name diagnostic'
expect_contains "$ERR" "./true" 'A4 bare name diagnostic suggests a fix'

printf 'not executable\n' > "$WORKDIR/not_exec"
chmod 000 "$WORKDIR/not_exec"
run run -- "$WORKDIR/not_exec"
expect_eq "$RC" 126 'A4 unreadable file gives 126'
expect_contains "$ERR" 'Permission denied' 'A4 permission diagnostic'

printf 'this is not a program\n' > "$WORKDIR/not_elf"
chmod 755 "$WORKDIR/not_elf"
run run -- "$WORKDIR/not_elf"
expect_eq "$RC" 126 'A4 non-executable format gives 126'
expect_contains "$ERR" 'Exec format error' 'A4 format diagnostic'

run run -- "$WORKDIR"
expect_eq "$RC" 126 'A4 a directory gives 126'

# --- argument passing -------------------------------------------------------
# The target's argv is the verbatim tail after '--', checked through a real
# exec rather than through retrace's own reporting.

run run -- "$FIXTURES/echo_args" hello 'a b c' --run ''
expect_eq "$RC" 0 'args: target ran'
expect_eq "$OUT" "argv[0] = $FIXTURES/echo_args
argv[1] = hello
argv[2] = a b c
argv[3] = --run
argv[4] = " 'args: argv passed through verbatim'

run run -- "$FIXTURES/echo_args" -- x
expect_contains "$OUT" 'argv[1] = --' 'args: a second -- belongs to the target'

# --- S1: syscall entry/exit tracing -----------------------------------------

printf 'hello from the file\n' > "$WORKDIR/in.txt"

run record -- "$FIXTURES/reader" "$WORKDIR/in.txt"
expect_eq "$RC" 0 'S1 record leaves the exit status alone'
expect_eq "$OUT" 'read 20 bytes: hello from the file' 'S1 target output is untouched'

# Named, not numbered: the syscall table has to be doing its job.
expect_contains "$ERR" 'openat(' 'S1 openat traced'
# Not the fd number: which descriptor the file lands on depends on what the
# harness left open, so assert only that arguments are rendered at all.
expect_contains "$ERR" 'read(0x'  'S1 read traced with arguments'
expect_contains "$ERR" 'write(0x1,' 'S1 write traced'
expect_contains "$ERR" 'exit_group(' 'S1 exit_group traced'

# Every syscall must produce an entry line and a matching return line, so the
# entry/exit alternation is what is being checked here, not just the count.
entries=$(grep -cE '^\[ *[0-9]+\] [^ =]' <<< "$ERR")
returns=$(grep -cE '^\[ *[0-9]+\] +=' <<< "$ERR")
expect_eq "$((entries - returns))" '1' 'S1 entries and returns pair up (exit_group never returns)'

# read(2) returned the file's 20 bytes, and the trace has to say so.
expect_contains "$ERR" '= 20 (0x14)' 'S1 read return value captured'

run record -- "$FIXTURES/crash"
expect_eq "$RC" 134 'S1 signals still work under syscall tracing'

# --- S2: the versioned binary log -------------------------------------------

LOG="$WORKDIR/session.log"
run record -o "$LOG" -- "$FIXTURES/reader" "$WORKDIR/in.txt"
expect_eq "$RC" 0 'S2 record with -o still runs the program'
expect_eq "$OUT" 'read 20 bytes: hello from the file' 'S2 target output untouched'

expect_eq "$(head -c 7 "$LOG")" 'RETRACE' 'S2 log carries its magic'

# The bytes read(2) returned have to be in the log, not just their count.
expect_contains "$(strings "$LOG")" 'hello from the file' 'S2 memory buffer captured'

OUT=$("$RETRACE" show "$LOG" 2>"$WORKDIR/stderr"); RC=$?; ERR=$(cat "$WORKDIR/stderr")
expect_eq "$RC" 0 'S2 show reads the log back'
expect_contains "$OUT" 'openat(' 'S2 show renders syscall names'
expect_contains "$OUT" 'bytes captured' 'S2 show reports the captured buffer'
expect_contains "$OUT" ' records' 'S2 show reports a record count'

# Versioning is only real if a mismatch is refused.
printf 'garbage' > "$WORKDIR/bad.log"
run show "$WORKDIR/bad.log"
expect_eq "$RC" 125 'S2 a non-log is refused'
expect_contains "$ERR" 'not a retrace log' 'S2 magic mismatch diagnosed'

cp "$LOG" "$WORKDIR/v9.log"
printf '\x09' | dd of="$WORKDIR/v9.log" bs=1 seek=8 conv=notrunc status=none
run show "$WORKDIR/v9.log"
expect_eq "$RC" 125 'S2 a future log version is refused'
expect_contains "$ERR" 'log version 9' 'S2 version mismatch diagnosed'

# --- S3: replay, injection and divergence -----------------------------------
# The log was recorded while in.txt said "hello from the file". Rewrite the
# file completely: a real run must see the new contents, a replay must see the
# recorded ones, because the bytes come from the log rather than the disk.

printf 'THE FILE NOW SAYS SOMETHING ELSE ENTIRELY\n' > "$WORKDIR/in.txt"

expect_eq "$("$FIXTURES/reader" "$WORKDIR/in.txt")" \
          'read 42 bytes: THE FILE NOW SAYS SOMETHING ELSE ENTIRELY' \
          'S3 a real run sees the new file contents'

run replay "$LOG" -- "$FIXTURES/reader" "$WORKDIR/in.txt"
expect_eq "$RC" 0 'S3 replay finishes cleanly'
expect_eq "$OUT" 'read 20 bytes: hello from the file' \
          'S3 replay feeds back the recorded bytes, not the file'

# Same log, different program: the syscall sequence stops matching.
run replay "$LOG" -- "$FIXTURES/echo_args" hello
expect_eq "$RC" 124 'S3 divergence exits 124'
expect_contains "$ERR" 'divergence at syscall' 'S3 divergence is reported'
expect_contains "$ERR" 'recorded openat, got' 'S3 divergence names both syscalls'

run replay "$WORKDIR/bad.log" -- "$FIXTURES/reader" "$WORKDIR/in.txt"
expect_eq "$RC" 125 'S3 replay refuses a log it cannot read'

# --- S4: breakpoints and reverse-continue -----------------------------------
# counter is built -no-pie, so the address nm reports for work() is the address
# it actually executes at.

WORK=0x$(nm "$FIXTURES/counter" | awk '$3=="work"{print $1}')
CLOG="$WORKDIR/counter.log"

# Record and replay must see the same kind of stdout. glibc probes it with
# ioctl(TCGETS) and buffers differently for a pipe, a file and a terminal, so
# recording through a pipe and replaying to /dev/null genuinely diverges.
# Every run below sends stdout to a regular file.
"$RETRACE" record -o "$CLOG" -- "$FIXTURES/counter" > "$WORKDIR/c.out" 2>/dev/null
expect_eq "$?" 0 'S4 counter recorded'
expect_eq "$(cat "$WORKDIR/c.out")" 'work 0
work 1
work 2' 'S4 counter output'

# Runs a replay with commands on stdin, stdout to a file, and returns stderr.
replay_with() {
    printf '%b' "$1" | "$RETRACE" replay --break "$WORK" "$CLOG" -- \
        "$FIXTURES/counter" 2>&1 > "$WORKDIR/c.out"
}

# work() is called three times, so the breakpoint must fire three times.
ERR=$(replay_with 'c\nc\nc\n')
expect_eq "$(grep -c 'stopped at' <<< "$ERR")" '3' 'S4 breakpoint hits three times'
expect_contains "$ERR" 'hit 3' 'S4 hits are counted'

# Registers must report the address stopped at, not one past the INT3.
ERR=$(replay_with 'r\nq\n')
expect_contains "$ERR" "rip 0x$(printf '%012x' "$WORK")" 'S4 regs reports rip at the breakpoint'

# Reverse-continue: reach hit 3, go back to hit 2, forward again to hit 3.
ERR=$(replay_with 'c\nc\nrc\nc\nq\n')
expect_eq "$(grep 'stopped at' <<< "$ERR" | grep -o 'hit [0-9]' | tr '\n' ' ')" \
          'hit 1 hit 2 hit 3 hit 2 hit 3 ' \
          'S4 reverse-continue steps back a hit, then forward again'

# There is nothing before the first hit to go back to.
ERR=$(replay_with 'rc\nq\n')
expect_contains "$ERR" 'already at the first hit' 'S4 reverse-continue refuses to go before hit 1'

run replay --break 0xnotanaddress "$CLOG" -- "$FIXTURES/counter"
expect_eq "$RC" 125 'S4 a bad breakpoint address is a usage error'

# --- A7: usage errors -------------------------------------------------------

run
expect_eq "$RC" 125 'A7 no arguments'
expect_contains "$ERR" 'retrace: no command given' 'A7 no arguments diagnostic'
expect_contains "$ERR" 'usage: retrace <command> [options] -- <program-path>' 'A7 usage text is shown'

run bogus -- true
expect_eq "$RC" 125 'A7 unknown subcommand'
expect_contains "$ERR" "retrace: unknown subcommand 'bogus'" 'A7 unknown subcommand diagnostic'

run run
expect_eq "$RC" 125 'A7 run with no program'
expect_contains "$ERR" 'retrace: no program specified' 'A7 no program diagnostic'

run run true
expect_eq "$RC" 125 'A7 run without separator'
expect_contains "$ERR" "retrace: expected '--' before the program" 'A7 separator diagnostic'

run run --
expect_eq "$RC" 125 'A7 run with bare --'
expect_contains "$ERR" "retrace: no program specified after '--'" 'A7 bare -- diagnostic'

# ---------------------------------------------------------------------------

printf '\n%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]
