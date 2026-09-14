#include "breakpoint.hpp"

#include <sys/wait.h>

namespace retrace {
namespace {

constexpr unsigned char kInt3 = 0xCC;

}  // namespace

bool Breakpoint::arm(pid_t pid, unsigned long address) {
    if (!read_memory(pid, address, &original_, 1)) return false;
    if (!write_memory(pid, address, &kInt3, 1)) return false;
    address_ = address;
    armed_ = true;
    return true;
}

bool Breakpoint::disarm(pid_t pid) {
    if (!armed_) return true;
    if (!write_memory(pid, address_, &original_, 1)) return false;
    armed_ = false;
    return true;
}

bool Breakpoint::is_hit(const Registers& regs) const {
    return armed_ && regs.rip == address_ + 1;
}

bool Breakpoint::rewind(pid_t pid, Registers* regs) const {
    regs->rip = address_;
    return set_regs(pid, *regs);
}

// rip is rewound on the hit itself, so by the time we get here it already
// points at the real instruction.
bool Breakpoint::step_over(pid_t pid) {
    if (!disarm(pid)) return false;

    if (!single_step(pid, 0)) return false;
    int status = 0;
    if (!wait_for(pid, &status)) return false;
    if (!WIFSTOPPED(status)) return false;  // the target died mid-step

    return arm(pid, address_);
}

}  // namespace retrace
