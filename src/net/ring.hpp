#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ring {

struct Descriptor {
    std::atomic<std::uint64_t> seq;
    const std::uint8_t* buf;
    std::uint32_t len;
    std::uint32_t flags;
    std::uint64_t hw_ts;
};
static_assert(sizeof(Descriptor) == 32);

struct View {
    const std::uint8_t* buf = nullptr;
    std::uint32_t len = 0;
    std::uint32_t flags = 0;
    std::uint64_t hw_ts = 0;
};

class Ring {
public:
    explicit Ring(std::size_t slots) : slots_(round_up(slots)), mask_(slots_ - 1) {
        d_ = std::vector<Descriptor>(slots_);
        for (auto& d : d_) d.seq.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t slots() const noexcept { return slots_; }

    void publish(const std::uint8_t* buf, std::uint32_t len, std::uint64_t hw_ts,
                 std::uint32_t flags = 0) noexcept {
        Descriptor& d = d_[next_ & mask_];
        d.buf = buf;
        d.len = len;
        d.flags = flags;
        d.hw_ts = hw_ts;
        d.seq.store(next_, std::memory_order_release);
        ++next_;
    }

    [[nodiscard]] std::uint64_t published() const noexcept { return next_ - 1; }

    enum class State : std::uint8_t { kReady, kWaiting, kLapped };

    [[nodiscard]] State take(std::uint64_t want, View* out) const noexcept {
        const Descriptor& d = d_[want & mask_];
        const std::uint64_t here = d.seq.load(std::memory_order_acquire);
        if (here == want) {
            __builtin_prefetch(d.buf, 0, 3);
            out->buf = d.buf;
            out->len = d.len;
            out->flags = d.flags;
            out->hw_ts = d.hw_ts;
            return State::kReady;
        }
        return here > want ? State::kLapped : State::kWaiting;
    }

private:
    static std::size_t round_up(std::size_t n) {
        std::size_t p = 2;
        while (p < n) p <<= 1;
        return p;
    }

    std::vector<Descriptor> d_;
    std::size_t slots_, mask_;
    std::uint64_t next_ = 1;
};

struct alignas(64) Cursor {
    std::uint64_t want = 1;
    char pad[56];
};
static_assert(sizeof(Cursor) == 64);

}
