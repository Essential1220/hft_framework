#pragma once
// ============================================
// async_writer.h - Abstract asynchronous file writer interface
// Implementations: StdioFileWriter (cross-platform), UringFileWriter (Linux)
// ============================================

#include <cstddef>
#include <memory>
#include <string>

namespace hft {

class IAsyncWriter {
public:
    virtual ~IAsyncWriter() = default;
    virtual bool open(const std::string& path) = 0;
    virtual bool write(const void* data, size_t len) = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
};

std::unique_ptr<IAsyncWriter> create_async_writer();

} // namespace hft
