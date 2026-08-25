#pragma once

// Reads one large file from beginning to end, for walking an ITCH file.
//
// Why not simply read a block and parse it: a record can straddle the boundary between two
// reads, with its first half at the end of one block and the rest at the start of the
// next. So the arrangement here is that the caller consumes only whole records and leaves
// the part record in the buffer; before the next read that remainder is moved to the
// front and the new data is added behind it.

// open and O_RDONLY.
#include <fcntl.h>
// fstat, for asking how large the file is when the capacity is left to this class.
#include <sys/stat.h>
// read, close and posix_fadvise.
#include <unistd.h>

// errno and EINTR, to tell a read interrupted by a signal from a real failure.
#include <cerrno>
// Byte sized reads and a 64 bit running total.
#include <cstdint>
// std::memmove for the part record. memmove and not memcpy: the source and the
// destination are in the same buffer and they overlap.
#include <cstring>
// std::vector, the buffer itself.
#include <vector>

namespace io {

class SeqReader {
public:
    // A capacity of zero means the whole file at once, for a caller that must not stop and
    // wait for a disk halfway through. An ITCH file is tens of gigabytes, which makes that
    // a real choice: two more minutes before the run is better than one disk read in the
    // middle of it.
    SeqReader(const char* path, std::size_t cap) {
        // A negative descriptor is what ok() below reports.
        fd_ = ::open(path, O_RDONLY);
        // Tells the kernel this will be read straight through, so it reads ahead. It
        // changes nothing about correctness, only speed.
        if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        struct stat st {};
        if (cap == 0 && fd_ >= 0 && ::fstat(fd_, &st) == 0) {
            // One byte more than the file, so a full buffer can never be mistaken for the
            // end of the file: reading zero bytes then really does mean the end.
            cap = static_cast<std::size_t>(st.st_size) + 1;
        }
        // resize rather than reserve, because the bytes below are written by index.
        buf_.resize(cap);
    }
    ~SeqReader() {
        if (fd_ >= 0) ::close(fd_);
    }
    // No copying: it holds a file descriptor, and a copy would close it twice.
    SeqReader(const SeqReader&) = delete;
    SeqReader& operator=(const SeqReader&) = delete;

    // Whether the file opened. The constructor does not throw, so this has to be asked.
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    // Fills the buffer.
    //
    // A false means one thing only: the file is finished and there is not a byte left in
    // the buffer. Anything still there, even half a record from last time, gives a true.
    bool fill() {
        while (!eof_ && len_ < buf_.size()) {
            // Behind whatever was left over, as much as will fit.
            const ssize_t n = ::read(fd_, buf_.data() + len_, buf_.size() - len_);
            if (n < 0) {
                // An interrupted read is not a failure; try again.
                if (errno == EINTR) continue;
                // A real failure is treated as the end of the file: what has already been
                // read is still valid and the caller can finish it.
                eof_ = true;
                break;
            }
            if (n == 0) {
                eof_ = true;
                break;
            }
            // One read often returns less than was asked for, which is why this adds up
            // inside a loop. Assuming one read fills the buffer is the classic version of
            // this bug.
            len_ += static_cast<std::size_t>(n);
            // Everything read from this file, printed at the end and used to check that all
            // of it was read.
            total_ += static_cast<std::uint64_t>(n);
        }
        // The test is whether the buffer holds anything, not whether the file is finished.
        // At the end of the file with half a record still in hand this has to return true,
        // or that half would be thrown away.
        return len_ > 0;
    }

    // The caller has finished with the first n bytes.
    void consume(std::size_t n) noexcept {
        // What is left moves to the front - the part record described at the top. memmove
        // because the two regions are the same buffer and overlap.
        if (n < len_) std::memmove(buf_.data(), buf_.data() + n, len_ - n);
        // This runs even when everything was consumed and the branch above did not: without
        // it the next fill would think the buffer was still full.
        len_ -= n;
    }

    // Where to start parsing.
    [[nodiscard]] const std::uint8_t* data() const noexcept { return buf_.data(); }
    // How many bytes are there to parse. It has to be asked again after every consume.
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    // Everything read so far. Compared with the size of the file at the end it says
    // whether the whole thing was read - which matters because a failed read above is
    // treated as the end rather than reported.
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_; }

private:
    int fd_ = -1;
    // Sized in the constructor and never resized.
    std::vector<std::uint8_t> buf_;
    // Valid bytes, counted from the front.
    std::size_t len_ = 0;
    std::uint64_t total_ = 0;
    bool eof_ = false;
};

}  // namespace io
