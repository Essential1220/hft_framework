#pragma once
// ============================================
// uring_file_writer.h - Linux io_uring asynchronous file writer
// Conditional compilation: only available on Linux with liburing.
// Falls back to StdioFileWriter on unsupported platforms.
// ============================================

#ifdef __linux__

#include "common/async_writer.h"

#include <atomic>
#include <cstring>
#include <vector>

struct io_uring;

namespace hft {

class UringFileWriter : public IAsyncWriter {
public:
    UringFileWriter();
    ~UringFileWriter() override;

    bool open(const std::string& path) override;
    bool write(const void* data, size_t len) override;
    void flush() override;
    void close() override;
    bool is_open() const override;

    static bool io_uring_available();

private:
    int fd_ = -1;
    io_uring* ring_ = nullptr;
    off_t offset_ = 0;
    bool open_ = false;
};

} // namespace hft

#endif // __linux__
