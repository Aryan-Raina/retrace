#pragma once

namespace retrace {

// nullptr for out-of-range numbers and the gaps in x86-64 numbering.
const char* syscall_name(long number);

}  // namespace retrace
