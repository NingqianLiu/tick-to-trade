#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace book {

class OrderMap {
public:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint8_t side;
    };

    explicit OrderMap(std::size_t orders) {
        std::size_t n = 16;
        while (n * 7 < orders * 10) n <<= 1;
        slots_.assign(n, Slot{});
        mask_ = n - 1;
        shift_ = 64;
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    bool insert(std::uint64_t oid, const Order& o) {
        if (o.shares == 0) return false;
        std::size_t i = start(oid);
        while (slots_[i].shares != 0) {
            if (slots_[i].oid == oid) {
                slots_[i].shares = o.shares;
                slots_[i].side_price = pack(o);
                return true;
            }
            i = (i + 1) & mask_;
        }
        if ((count_ + 1) * 10 > slots_.size() * 7) return false;
        slots_[i] = Slot{oid, o.shares, pack(o)};
        ++count_;
        return true;
    }

    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *out = unpack(slots_[i]);
        return true;
    }

    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *before = unpack(slots_[i]);
        if (shares >= slots_[i].shares) {
            remove(i);
        } else {
            slots_[i].shares -= shares;
        }
        return true;
    }

    bool erase(std::uint64_t oid, Order* gone) {
        const std::size_t i = probe(oid);
        if (i == kMissing) return false;
        *gone = unpack(slots_[i]);
        remove(i);
        return true;
    }

private:
    struct Slot {
        std::uint64_t oid = 0;
        std::uint32_t shares = 0;
        std::uint32_t side_price = 0;
    };
    static_assert(sizeof(Slot) == 16);

    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    static std::uint32_t pack(const Order& o) {
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    static Order unpack(const Slot& s) {
        return Order{s.shares, s.side_price & 0x7fffffffu,
                     static_cast<std::uint8_t>(s.side_price >> 31)};
    }

    [[nodiscard]] std::size_t start(std::uint64_t oid) const noexcept {
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    [[nodiscard]] std::size_t probe(std::uint64_t oid) const noexcept {
        std::size_t i = start(oid);
        while (slots_[i].shares != 0) {
            if (slots_[i].oid == oid) return i;
            i = (i + 1) & mask_;
        }
        return kMissing;
    }

    void remove(std::size_t hole) {
        std::size_t j = hole;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].shares == 0) break;
            const std::size_t want = start(slots_[j].oid);
            const bool between = hole <= j ? (hole < want && want <= j)
                                           : (hole < want || want <= j);
            if (between) continue;
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole] = Slot{};
        --count_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0, count_ = 0;
    int shift_ = 64;
};

}
