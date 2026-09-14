#include "log.hpp"

#include "syscalls.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#include <sys/syscall.h>

namespace retrace {

int buffer_arg_of(long nr) {
    switch (nr) {
        case SYS_read:
        case SYS_pread64:
        case SYS_getrandom:
        case SYS_getdents64:
        case SYS_readlink:
        case SYS_recvfrom:
            return 1;
        case SYS_uname:
            return 0;
        default:
            return -1;
    }
}

std::string render_call(const Record& record) {
    const char* name = syscall_name(record.head.nr);
    std::string line = name != nullptr ? name
                                       : std::format("syscall_{}", record.head.nr);
    line += '(';
    for (int i = 0; i < 6; ++i) {
        if (i > 0) line += ", ";
        line += std::format("0x{:x}",
                            static_cast<unsigned long>(record.head.args[i]));
    }
    return line + ')';
}

std::string render_result(const Record& record) {
    const int64_t ret = record.head.ret;
    // Returns in [-4095, -1] are -errno rather than a value.
    std::string line =
        (ret < 0 && ret >= -4095)
            ? std::format("-1 {}", std::strerror(static_cast<int>(-ret)))
            : std::format("{} (0x{:x})", ret, static_cast<unsigned long>(ret));
    if (record.head.buf_len > 0) {
        line += std::format("  [{} bytes captured]", record.head.buf_len);
    }
    return line;
}

bool LogWriter::open(const char* path) {
    file_ = std::fopen(path, "wb");
    if (file_ == nullptr) {
        complain(std::format("{}: {}", path, std::strerror(errno)));
        return false;
    }
    const LogHeader header{{'R', 'E', 'T', 'R', 'A', 'C', 'E', '\0'},
                           kVersion, kArchX86_64, 0};
    std::fwrite(&header, sizeof(header), 1, file_);
    return true;
}

void LogWriter::append(const Record& record) {
    if (file_ == nullptr) return;
    std::fwrite(&record.head, sizeof(record.head), 1, file_);
    if (record.head.buf_len > 0) {
        std::fwrite(record.buf.data(), 1, record.head.buf_len, file_);
    }
    ++count_;
}

void LogWriter::close() {
    if (file_ == nullptr) return;
    // The count is only known at the end, so seek back and fill it in.
    std::fseek(file_, offsetof(LogHeader, count), SEEK_SET);
    std::fwrite(&count_, sizeof(count_), 1, file_);
    std::fclose(file_);
    file_ = nullptr;
}

bool read_log(const char* path, std::vector<Record>* records) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        complain(std::format("{}: {}", path, std::strerror(errno)));
        return false;
    }

    LogHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.magic, "RETRACE", 8) != 0) {
        complain(std::format("{}: not a retrace log", path));
        std::fclose(file);
        return false;
    }
    if (header.version != kVersion) {
        complain(std::format("{}: log version {}, this build reads version {}",
                             path, header.version, kVersion));
        std::fclose(file);
        return false;
    }

    Record record;
    while (std::fread(&record.head, sizeof(record.head), 1, file) == 1) {
        record.buf.assign(record.head.buf_len, '\0');
        if (record.head.buf_len > 0 &&
            std::fread(record.buf.data(), 1, record.head.buf_len, file) !=
                record.head.buf_len) {
            complain(std::format("{}: truncated record {}", path, records->size()));
            std::fclose(file);
            return false;
        }
        records->push_back(record);
    }
    std::fclose(file);

    if (records->size() != header.count) {
        complain(std::format("{}: header says {} records, found {}", path,
                             header.count, records->size()));
        return false;
    }
    return true;
}

}  // namespace retrace
