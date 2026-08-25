#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/sha256.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace book {

class RefBook {
public:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint16_t locate;
        char side;
    };

    struct Counters {
        std::uint64_t added = 0;
        std::uint64_t executed = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t deleted = 0;
        std::uint64_t replaced = 0;
        std::uint64_t orphan_ref = 0;
        std::uint64_t oversized_exec = 0;
        std::uint64_t duplicate_ref = 0;
    };

    explicit RefBook(std::size_t reserve = 1u << 21) { orders_.reserve(reserve); }

    void apply(const itch::Message& m) {
        switch (m.type()) {
            case 'A':
            case 'F':
                add(m);
                break;
            case 'E':
            case 'C':
                ++c_.executed;
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kExecRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kExecSharesOff));
                break;
            case 'X':
                ++c_.cancelled;
                reduce(itch::read_be<std::uint64_t>(m.body + itch::kCancelRefOff),
                       itch::read_be<std::uint32_t>(m.body + itch::kCancelSharesOff));
                break;
            case 'D':
                ++c_.deleted;
                if (orders_.erase(itch::read_be<std::uint64_t>(
                        m.body + itch::kDeleteRefOff)) == 0) {
                    ++c_.orphan_ref;
                }
                break;
            case 'U':
                replace(m);
                break;
            default:
                break;
        }
    }

    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }

    [[nodiscard]] std::vector<std::uint32_t> live_by_locate() const {
        std::vector<std::uint32_t> v(1u << 16, 0);
        for (const auto& [ref, o] : orders_) ++v[o.locate];
        return v;
    }

    [[nodiscard]] std::size_t peak_live() const noexcept { return peak_live_; }
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }

    [[nodiscard]] std::string hash() const {
        struct Row {
            std::uint16_t locate;
            char side;
            std::uint32_t price;
            std::uint64_t ref;
            std::uint32_t shares;
        };
        std::vector<Row> rows;
        rows.reserve(orders_.size());
        for (const auto& [ref, o] : orders_) {
            rows.push_back({o.locate, o.side, o.price, ref, o.shares});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.locate != b.locate) return a.locate < b.locate;
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price) return a.price < b.price;
            return a.ref < b.ref;
        });

        crypto::Sha256 sha;
        std::uint8_t rec[19];
        for (const Row& r : rows) {
            put_be(rec + 0, r.locate, 2);
            rec[2] = static_cast<std::uint8_t>(r.side);
            put_be(rec + 3, r.price, 4);
            put_be(rec + 7, r.ref, 8);
            put_be(rec + 15, r.shares, 4);
            sha.update(rec, sizeof(rec));
        }
        return sha.hex();
    }

private:
    static void put_be(std::uint8_t* p, std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
        }
    }

    void add(const itch::Message& m) {
        const std::uint64_t ref =
            itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        const Order o{itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
                      itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff),
                      m.stock_locate(),
                      static_cast<char>(m.body[itch::kAddSideOff])};
        const auto [it, inserted] = orders_.emplace(ref, o);
        if (!inserted) {
            ++c_.duplicate_ref;
            it->second = o;
        }
        ++c_.added;
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    void reduce(std::uint64_t ref, std::uint32_t shares) {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) {
            ++c_.orphan_ref;
            return;
        }
        if (shares >= it->second.shares) {
            if (shares > it->second.shares) ++c_.oversized_exec;
            orders_.erase(it);
        } else {
            it->second.shares -= shares;
        }
    }

    void replace(const itch::Message& m) {
        ++c_.replaced;
        const std::uint64_t old_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff);
        const auto it = orders_.find(old_ref);
        if (it == orders_.end()) {
            ++c_.orphan_ref;
            return;
        }
        Order o = it->second;
        orders_.erase(it);
        o.shares = itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff);
        o.price = itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff);
        const std::uint64_t new_ref =
            itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff);
        if (!orders_.emplace(new_ref, o).second) ++c_.duplicate_ref;
        peak_live_ = std::max(peak_live_, orders_.size());
    }

    std::unordered_map<std::uint64_t, Order> orders_;
    Counters c_;
    std::size_t peak_live_ = 0;
};

}
