#pragma once

#include "retrace.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace retrace {

constexpr uint32_t kVersion = 1;
constexpr uint32_t kArchX86_64 = 1;

struct LogHeader {
    char magic[8];      // "RETRACE"
    uint32_t version;
    uint32_t arch;
    uint64_t count;     // patched when the writer closes
};

// What goes on disk ahead of each record's captured bytes.
struct RecordHeader {
    int64_t nr;
    int64_t args[6];
    int64_t ret;
    Registers regs;     // full register state at syscall exit
    uint32_t buf_len;   // captured bytes following this header
    int32_t buf_arg;    // argument index that held the buffer, -1 if none
};

// The in-memory form: the same fields, with the bytes attached.
struct Record {
    RecordHeader head{};
    std::vector<char> buf;
};

class LogWriter {
public:
    bool open(const char* path);
    void append(const Record& record);
    void close();
    ~LogWriter() { close(); }

private:
    FILE* file_ = nullptr;
    uint64_t count_ = 0;
};

// Complains and returns false if the magic or version does not match.
bool read_log(const char* path, std::vector<Record>* records);

// Syscalls whose result lands in a caller-supplied buffer: returns the
// argument index holding that pointer, or -1.
int buffer_arg_of(long nr);

// Shared by `record` and `show`, so a log reads back the way it was printed.
std::string render_call(const Record& record);
std::string render_result(const Record& record);

}  // namespace retrace
