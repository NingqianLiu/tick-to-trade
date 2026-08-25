#pragma once

#include <cstddef>
#include <cstdint>

#include "common/huge.hpp"

namespace book {

class PoolMap {
public:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint8_t side;
    };

    explicit PoolMap(std::size_t orders) {
        std::size_t n = 16;
        while (n < orders) n <<= 1;
        buckets_.assign(n, kEnd);
        shift_ = 64;
        for (std::size_t c = n; c > 1; c >>= 1) --shift_;

        nodes_.resize(orders < 16 ? 16 : orders);
        for (std::uint32_t i = 0; i + 1 < nodes_.size(); ++i) {
            nodes_[i].next = i + 1;
        }
        nodes_.back().next = kEnd;
        free_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return nodes_.size(); }

    bool insert(std::uint64_t oid, const Order& o) {
        if (o.shares == 0) return false;
        const std::size_t b = bucket(oid);
        for (std::uint32_t i = buckets_[b]; i != kEnd; i = nodes_[i].next) {
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
        nodes_[i].next = buckets_[b];
        buckets_[b] = i;
        ++count_;
        return true;
    }

    void ask_for_all(const std::uint64_t* oids, std::size_t n) const {
        for (std::size_t j = 0; j < n; ++j) {
            __builtin_prefetch(&buckets_[bucket(oids[j])], 0, 3);
        }
        for (std::size_t j = 0; j < n; ++j) {
            const std::uint32_t i = buckets_[bucket(oids[j])];
            if (i != kEnd) __builtin_prefetch(&nodes_[i], 0, 3);
        }
    }

    [[nodiscard]] bool find(std::uint64_t oid, Order* out) const {
        for (std::uint32_t i = buckets_[bucket(oid)]; i != kEnd; i = nodes_[i].next) {
            if (nodes_[i].oid == oid) {
                *out = unpack(nodes_[i]);
                return true;
            }
        }
        return false;
    }

    bool reduce(std::uint64_t oid, std::uint32_t shares, Order* before) {
        const std::size_t b = bucket(oid);
        std::uint32_t* link = &buckets_[b];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *before = unpack(nodes_[i]);
            if (shares >= nodes_[i].shares) {
                unlink(link, i);
            } else {
                nodes_[i].shares -= shares;
            }
            return true;
        }
        return false;
    }

    bool erase(std::uint64_t oid, Order* gone) {
        std::uint32_t* link = &buckets_[bucket(oid)];
        for (std::uint32_t i = *link; i != kEnd; link = &nodes_[i].next, i = *link) {
            if (nodes_[i].oid != oid) continue;
            *gone = unpack(nodes_[i]);
            unlink(link, i);
            return true;
        }
        return false;
    }

private:
    struct Node {
        std::uint64_t oid = 0;
        std::uint32_t shares = 0;
        std::uint32_t side_price = 0;
        std::uint32_t next = 0;
    };

    static constexpr std::uint32_t kEnd = 0xffffffffu;

    static std::uint32_t pack(const Order& o) {
        return (static_cast<std::uint32_t>(o.side) << 31) | o.price;
    }
    static Order unpack(const Node& n) {
        return Order{n.shares, n.side_price & 0x7fffffffu,
                     static_cast<std::uint8_t>(n.side_price >> 31)};
    }

    [[nodiscard]] std::size_t bucket(std::uint64_t oid) const noexcept {
        return (oid * 0x9E3779B97F4A7C15ull) >> shift_;
    }

    void unlink(std::uint32_t* link, std::uint32_t i) {
        *link = nodes_[i].next;
        nodes_[i].next = free_;
        free_ = i;
        --count_;
    }

    huge::Buffer<Node> nodes_;
    huge::Buffer<std::uint32_t> buckets_;
    std::size_t count_ = 0;
    std::uint32_t free_ = kEnd;
    int shift_ = 64;
};

}
