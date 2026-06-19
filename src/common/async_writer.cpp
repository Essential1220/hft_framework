// ============================================
// async_writer.cpp - Factory for IAsyncWriter
// Returns UringFileWriter on Linux when io_uring is available,
// otherwise falls back to StdioFileWriter.
// ============================================

#include "common/async_writer.h"
#include "common/stdio_file_writer.h"

#ifdef __linux__
#include "common/uring_file_writer.h"
#endif

namespace hft {

std::unique_ptr<IAsyncWriter> create_async_writer() {
#ifdef __linux__
    if (UringFileWriter::io_uring_available()) {
        return std::make_unique<UringFileWriter>();
    }
#endif
    return std::make_unique<StdioFileWriter>();
}

} // namespace hft
