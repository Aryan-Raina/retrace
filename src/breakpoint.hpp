#pragma once

#include "retrace.hpp"

namespace retrace {

// A software breakpoint: the byte at `address` is replaced with 0xCC, the
// x86 INT3 instruction, so executing it traps into the kernel and stops the
// tracee. The displaced byte is kept so it can be put back.
class Breakpoint {
public:
    bool arm(pid_t pid, unsigned long address);
    bool disarm(pid_t pid);

    // After INT3 executes, rip points just past it, so a hit is rip - 1.
    bool is_hit(const Registers& regs) const;

    // INT3 leaves rip one byte past the breakpoint. Wind it back so the
    // tracee, and anything that inspects it, sees the instruction it stopped
    // at rather than the one after.
    bool rewind(pid_t pid, Registers* regs) const;

    // Put the real instruction back, run exactly it, then re-arm. Without the
    // single step the breakpoint would immediately trap itself again.
    bool step_over(pid_t pid);

    unsigned long address() const { return address_; }

private:
    unsigned long address_ = 0;
    unsigned char original_ = 0;
    bool armed_ = false;
};

}  // namespace retrace
