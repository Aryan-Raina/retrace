#pragma once

#include "log.hpp"

#include <vector>

namespace retrace {

// Exit code used when the target's syscall sequence stops matching the log.
constexpr int kExitDiverged = 124;

struct ReplayRequest {
    unsigned long breakpoint = 0;  // 0 means no breakpoint
    unsigned long stop_at = 1;     // which hit of it to stop on, counting from 1
};

struct ReplayOutcome {
    int code = 0;
    // Reverse-continue cannot rewind a live process, so it asks the caller to
    // launch a fresh one and replay to an earlier hit. Deterministic replay is
    // what makes that land in the same place.
    bool restart = false;
    unsigned long stop_at = 0;
};

// Runs the target against a recording: at every syscall it checks the target
// asked for the same thing as last time, and feeds back the recorded answer
// instead of letting the kernel do the work.
ReplayOutcome replay(pid_t pid, const std::vector<Record>& records,
                     const ReplayRequest& request);

}  // namespace retrace
