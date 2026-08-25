#pragma once

// A small tool for reading one large file from beginning to end in order, for reading ITCH market
// data files in particular.
//
// Why "read a block, parse a block" will not simply do: a record very likely straddles the
// boundary of two reads - its first half at the end of one block and its second at the start of
// the next.
// So the arrangement here is: the caller eats only complete records, and the half record left
// stays in the buffer; before the next read that half is moved to the front of the buffer and new
// data is filled in behind it.

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
    // A capacity of zero means "read the whole file in at once".
    // It is for a caller that cannot stop halfway to wait for a disk.
    // An ITCH file is tens of gigabytes, so that is a genuine choice.
    // Better to wait two more minutes before a run than to be interrupted by one disk read in the
    // middle of it.
    SeqReader(const char* path, std::size_t cap) {
        // Opened read only. On a failure fd_ is negative and ok() below returns false.
        fd_ = ::open(path, O_RDONLY);
        // Telling the kernel "I am reading this from beginning to end in order" makes it read
        // ahead.
        // This line changes no correctness and only affects how fast the reading is.
        if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        // fstat fills this in, and the file's size is one of its fields.
        struct stat st {};
        // A capacity of zero with the file open asks how large the file is.
        if (cap == 0 && fd_ >= 0 && ::fstat(fd_, &st) == 0) {
            // One byte more is asked for. That way fill() can never face "the buffer is exactly
            // full while the file is not finished", and reading zero bytes certainly means the
            // end.
            cap = static_cast<std::size_t>(st.st_size) + 1;
        }
        // The buffer is opened.
        // resize rather than reserve, because what follows writes into it by index.
        buf_.resize(cap);
    }
    // Destruction: close the file. The buffer is returned by the vector itself.
    ~SeqReader() {
        if (fd_ >= 0) ::close(fd_);
    }
    // No copying: it holds a file descriptor, and a copy would close it twice.
    SeqReader(const SeqReader&) = delete;
    // No copy assignment either, for the same reason.
    SeqReader& operator=(const SeqReader&) = delete;

    // Whether the file opened. The constructor throws nothing, so this has to be asked.
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    // Fills the buffer.
    // A false means one thing only: the file is finished and not one byte is left in the buffer.
    // Anything left gives a true, even if it is only half a record from last time.
    bool fill() {
        // Not at the end of the file and the buffer not full, so it carries on reading.
        while (!eof_ && len_ < buf_.size()) {
            // Filling in behind what was left from last time, as much as will go.
            const ssize_t n = ::read(fd_, buf_.data() + len_, buf_.size() - len_);
            // A negative is an error rather than "read zero bytes". The two have to be handled
            // apart.
            if (n < 0) {
                // Being interrupted by a signal is not an error and only needs another try.
                if (errno == EINTR) continue;
                // A real error. Treated as the end of the file - what has been read is still
                // valid and the caller can still eat it.
                eof_ = true;
                break;
            }
            // Reading zero bytes really is the end of the file.
            if (n == 0) {
                eof_ = true;
                break;
            }
            // One read very likely does not fill it, so this adds on and is wrapped in a loop -
            // taking one read as having filled it is the most common bug of this kind.
            len_ += static_cast<std::size_t>(n);
            // How many bytes have been read from this file altogether. Printed at the end, and
            // used to check whether the file was read whole.
            total_ += static_cast<std::uint64_t>(n);
        }
        // The test is "is there anything left in the buffer" rather than "is the file finished".
        // With the file at its end but half a record left in the buffer, this has to return true,
        // or that half would be thrown away.
        return len_ > 0;
    }

    // The caller ate the first n bytes and says so.
    void consume(std::size_t n) noexcept {
        // The part not eaten is moved to the front of the buffer - which is the "half record"
        // handling named at the top of this file. It has to be memmove: the source and the
        // destination are the same memory and they overlap.
        if (n < len_) std::memmove(buf_.data(), buf_.data() + n, len_ - n);
        // With all of it eaten the if above does not hold (there is nothing to move), but this
        // line still has to run and take len_ down to zero - missing it, the next fill would
        // think the buffer was still full.
        len_ -= n;
    }

    // Where the buffer starts. The caller parses from here.
    [[nodiscard]] const std::uint8_t* data() const noexcept { return buf_.data(); }
    // How many bytes from data() may be parsed. The caller has to ask again on every parse,
    // because this number changes after a consume.
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    // How many bytes have been read altogether from the start.
    // Compared with the file's size at the end, it says whether it was really read through.
    // Where a read failed halfway, it was treated as the end above and reported nothing.
    [[nodiscard]] std::uint64_t total_bytes() const noexcept { return total_; }

private:
    // The file descriptor. A negative means it did not open.
    int fd_ = -1;
    // The buffer itself, opened to its capacity at construction and never changed after.
    std::vector<std::uint8_t> buf_;
    // How many valid bytes the buffer holds, counting from the start.
    std::size_t len_ = 0;
    // How many bytes have been read from the file altogether.
    std::uint64_t total_ = 0;
    // Whether the file has reached its end.
    bool eof_ = false;
};

}  // namespace io
