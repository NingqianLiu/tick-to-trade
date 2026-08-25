#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

namespace io {

class SeqReader {
public:
    SeqReader(const char* path, std::size_t cap) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        struct stat st {};
        if (cap == 0 && fd_ >= 0 && ::fstat(fd_, &st) == 0) {
            cap = static_cast<std::size_t>(st.st_size) + 1;
        }
        buf_.resize(cap);
    }
    ~SeqReader() {
        if (fd_ >= 0) ::close(fd_);
    }
    SeqReader(const SeqReader&) = delete;
    SeqReader& operator=(const SeqReader&) = delete;

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    bool fill() {
        while (!eof_ && len_ < buf_.size()) {
            const ssize_t n = ::read(fd_, buf_.data() + len_, buf_.size() - len_);
            if (n < 0) {
                if (errno == EINTR) continue;
                eof_ = true;
                break;
            }
            if (n == 0) {
                eof_ = true;
                break;
            }
            len_ += static_cast<std::size_t>(n);
            total_ += static_cast<std::uint64_t>(n);
        }
        return len_ > 0;
    }

    void consume(std::size_t n) noexcept {
        if (n < len_) std::memmove(buf_.data(), buf_.data() + n, len_ - n);
        len_ -= n;
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept { return buf_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_; }

private:
    int fd_ = -1;
    std::vector<std::uint8_t> buf_;
    std::size_t len_ = 0;
    std::uint64_t total_ = 0;
    bool eof_ = false;
};

}
