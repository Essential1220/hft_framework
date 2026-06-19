#pragma once
// ============================================
// stdio_file_writer.h - Cross-platform IAsyncWriter via FILE*
// Fallback implementation that works everywhere.
// ============================================

#include "common/async_writer.h"
#include <cstdio>

namespace hft {

class StdioFileWriter : public IAsyncWriter {
public:
    ~StdioFileWriter() override { close(); }

    bool open(const std::string& path) override {
        close();
#ifdef _WIN32
        fopen_s(&fp_, path.c_str(), "ab");
#else
        fp_ = std::fopen(path.c_str(), "ab");
#endif
        return fp_ != nullptr;
    }

    bool write(const void* data, size_t len) override {
        if (!fp_) return false;
        return std::fwrite(data, 1, len, fp_) == len;
    }

    void flush() override {
        if (fp_) std::fflush(fp_);
    }

    void close() override {
        if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    }

    bool is_open() const override { return fp_ != nullptr; }

private:
    FILE* fp_ = nullptr;
};

} // namespace hft
