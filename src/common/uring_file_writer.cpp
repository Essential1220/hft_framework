// ============================================
// uring_file_writer.cpp - Linux io_uring file writer implementation
// ============================================

#ifdef __linux__

#include "common/uring_file_writer.h"

#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft {

static constexpr unsigned kQueueDepth = 64;

UringFileWriter::UringFileWriter() = default;

UringFileWriter::~UringFileWriter() {
    close();
}

bool UringFileWriter::io_uring_available() {
    struct io_uring probe_ring{};
    int ret = io_uring_queue_init(2, &probe_ring, 0);
    if (ret < 0) return false;
    io_uring_queue_exit(&probe_ring);
    return true;
}

bool UringFileWriter::open(const std::string& path) {
    close();
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) return false;

    ring_ = new io_uring{};
    int ret = io_uring_queue_init(kQueueDepth, ring_, 0);
    if (ret < 0) {
        ::close(fd_); fd_ = -1;
        delete ring_; ring_ = nullptr;
        return false;
    }

    struct stat st{};
    if (fstat(fd_, &st) == 0) offset_ = st.st_size;
    open_ = true;
    return true;
}

bool UringFileWriter::write(const void* data, size_t len) {
    if (!open_ || !ring_) return false;

    struct io_uring_sqe* sqe = io_uring_get_sqe(ring_);
    if (!sqe) {
        flush();
        sqe = io_uring_get_sqe(ring_);
        if (!sqe) return false;
    }

    io_uring_prep_write(sqe, fd_, data, static_cast<unsigned>(len), offset_);
    sqe->user_data = len;
    offset_ += static_cast<off_t>(len);

    io_uring_submit(ring_);
    return true;
}

void UringFileWriter::flush() {
    if (!open_ || !ring_) return;
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(ring_, &cqe) == 0) {
        io_uring_cqe_seen(ring_, cqe);
    }
    fdatasync(fd_);
}

void UringFileWriter::close() {
    if (!open_) return;
    flush();
    if (ring_) {
        io_uring_queue_exit(ring_);
        delete ring_;
        ring_ = nullptr;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    open_ = false;
    offset_ = 0;
}

bool UringFileWriter::is_open() const {
    return open_;
}

} // namespace hft

#endif // __linux__
