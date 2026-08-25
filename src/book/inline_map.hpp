#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace book {

class InlineMap {
public:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint8_t side;
    };

    explicit InlineMap(std::size_t orders) {
        std::size_t n = 16;
        while (n < orders * 2) n <<= 1;
        buckets_.assign(n, Slot{});
        nodes_.resize(orders < 16 ? 16 : orders);
        shift_ = 64;
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;
        for (std::size_t i = 0; i + 1 < nodes_.size(); ++i) {
            nodes_[i].next = static_cast<std::uint32_t>(i + 1);
        }
        nodes_.back().next = kEnd;
        free_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    bool insert(std::uint64_t oid, const Order& o) {
        if (o.shares == 0) return false;
        Slot& s = buckets_[bucket(oid)];
        if (s.shares == 0) {
            s.oid = oid;
            s.shares = o.shares;
            s.side_price = pack(o);
            ++count_;
            return true;
        }
        if (s.oid == oid) {
            s.shares = o.shares;
            s.side_price = pack(o);
            return true;
        }
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                nodes_[i].shares = o.shares;
                nodes_[i].side_price = pack(o);
                return true;
            }
        }
        if (free_ == kEnd) return false;
        const std::uint32_t i = free_;
        free_ = nodes_[i].next;
        nodes_[i].oid = oid;
        nodes_[i].shares = o.shares;
        nodes_[i].side_price = pack(o);
        nodes_[i].next = s.next;
        s.next = i;
        ++count_;
        return true;
    }

    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        const Slot& s = buckets_[bucket(oid)];
        if (s.shares != 0 && s.oid == oid) {
            *out = unpack(s.shares, s.side_price);
            return true;
        }
        for (std::uint32_t i = s.next; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                *out = unpack(nodes_[i].shares, nodes_[i].side_price);
                return true;
            }
        }
        return false;
    }

    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        Slot& s = buckets_[bucket(oid)];
        if (s.shares != 0 && s.oid == oid) {
            *before = unpack(s.shares, s.side_price);
            if (shares >= s.shares) {
                lift(s);
            } else {
                s.shares -= shares;
            }
            return true;
        }
        std::uint32_t* link = &s.next;
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *before = unpack(nodes_[i].shares, nodes_[i].side_price);
            if (shares >= nodes_[i].shares) {
                *link = nodes_[i].next;
                nodes_[i].next = free_;
                free_ = i;
                --count_;
            } else {
                nodes_[i].shares -= shares;
            }
            return true;
        }
        return false;
    }

    bool erase(std::uint64_t oid, Order* gone) {
        Slot& s = buckets_[bucket(oid)];
        if (s.shares != 0 && s.oid == oid) {
            *gone = unpack(s.shares, s.side_price);
            lift(s);
            return true;
        }
        std::uint32_t* link = &s.next;
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *gone = unpack(nodes_[i].shares, nodes_[i].side_price);
            *link = nodes_[i].next;
            nodes_[i].next = free_;
            free_ = i;
            --count_;
            return true;
        }
        return false;
    }

    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        for (std::size_t j = 0; j < n; ++j) {
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
    }

private:
    static constexpr std::uint32_t kEnd = 0xffffffffu;

    struct Slot {
        std::uint64_t oid = 0;
        std::uint32_t shares = 0;
        std::uint32_t side_price = 0;
        std::uint32_t next = kEnd;
        std::uint32_t pad = 0;
    };

    struct Node {
        std::uint64_t oid = 0;
        std::uint32_t shares = 0;
        std::uint32_t side_price = 0;
        std::uint32_t next = kEnd;
        std::uint32_t pad = 0;
    };

    static std::uint32_t pack(const Order& o) {
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    static Order unpack(std::uint32_t shares, std::uint32_t sp) {
        return Order{shares, sp & 0x7fffffffu, static_cast<std::uint8_t>(sp >> 31)};
    }

    void lift(Slot& s) {
        --count_;
        const std::uint32_t i = s.next;
        if (i == kEnd) {
            s.oid = 0;
            s.shares = 0;
            s.side_price = 0;
            return;
        }
        s.oid = nodes_[i].oid;
        s.shares = nodes_[i].shares;
        s.side_price = nodes_[i].side_price;
        s.next = nodes_[i].next;
        nodes_[i].next = free_;
        free_ = i;
    }

    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    std::vector<Slot> buckets_;
    std::vector<Node> nodes_;
    std::uint32_t free_ = kEnd;
    std::size_t count_ = 0;
    int shift_ = 64;
};

}
