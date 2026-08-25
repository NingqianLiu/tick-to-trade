#pragma once

#include <cstdint>
#include <vector>

#include "book/pool_map.hpp"
#include "book/price_levels.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace book {

class OrderBook {
public:
    struct Counters {
        std::uint64_t added = 0;
        std::uint64_t executed = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t deleted = 0;
        std::uint64_t replaced = 0;
        std::uint64_t orphan = 0;
        std::uint64_t oversized = 0;
        std::uint64_t full = 0;
        std::uint64_t duplicate = 0;
        std::uint64_t untracked = 0;
    };

    OrderBook(std::size_t orders, const std::vector<std::uint32_t>& references)
        : orders_(orders), levels_(references) {}

    OrderBook(std::size_t orders, std::size_t level_words)
        : orders_(orders), levels_(level_words) {}

    bool bind(std::uint16_t sym, std::uint32_t reference) {
        return levels_.bind(sym, reference);
    }

    bool apply(const itch::Message& m, std::uint16_t* sym) {
        *sym = m.stock_locate();
        switch (m.type()) {
            case 'A':
            case 'F':
                return add(m);
            case 'E':
                return take(m, itch::kExecRefOff, itch::kExecSharesOff, &c_.executed);
            case 'C':
                return take(m, itch::kExecRefOff, itch::kExecSharesOff, &c_.executed);
            case 'X':
                return take(m, itch::kCancelRefOff, itch::kCancelSharesOff, &c_.cancelled);
            case 'D':
                return remove(m);
            case 'U':
                return replace(m, sym);
            default:
                return false;
        }
    }

    [[nodiscard]] std::uint64_t top3(std::uint16_t sym, std::uint8_t side) const {
        return levels_.top3(sym, side);
    }
    [[nodiscard]] bool best(std::uint16_t sym, std::uint8_t side, std::uint32_t* price,
                            std::uint32_t* shares) const {
        return levels_.best(sym, side, price, shares);
    }
    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }
    [[nodiscard]] const PriceLevels& levels() const noexcept { return levels_; }

private:
    static std::uint8_t side_of(const itch::Message& m) {
        return m.body[itch::kAddSideOff] == 'B' ? PriceLevels::kBuy : PriceLevels::kSell;
    }

    bool add(const itch::Message& m) {
        const std::uint64_t oid = itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        const PoolMap::Order o{
            itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff), side_of(m)};
        ++c_.added;
        PoolMap::Order old{};
        if (orders_.find(oid, &old)) {
            ++c_.duplicate;
            levels_.remove(m.stock_locate(), old.side, old.price, old.shares);
        }
        if (!orders_.insert(oid, o)) {
            ++c_.full;
            return false;
        }
        put(m.stock_locate(), o);
        return true;
    }

    bool take(const itch::Message& m, std::size_t ref_off, std::size_t shares_off,
              std::uint64_t* counter) {
        ++*counter;
        const std::uint64_t oid = itch::read_be<std::uint64_t>(m.body + ref_off);
        const std::uint32_t want = itch::read_be<std::uint32_t>(m.body + shares_off);
        PoolMap::Order before{};
        if (!orders_.reduce(oid, want, &before)) {
            ++c_.orphan;
            return false;
        }
        if (want > before.shares) ++c_.oversized;
        const std::uint32_t took = want < before.shares ? want : before.shares;
        levels_.remove(m.stock_locate(), before.side, before.price, took);
        return true;
    }

    bool remove(const itch::Message& m) {
        ++c_.deleted;
        PoolMap::Order gone{};
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kDeleteRefOff),
                           &gone)) {
            ++c_.orphan;
            return false;
        }
        levels_.remove(m.stock_locate(), gone.side, gone.price, gone.shares);
        return true;
    }

    bool replace(const itch::Message& m, std::uint16_t* sym) {
        ++c_.replaced;
        PoolMap::Order gone{};
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff),
                           &gone)) {
            ++c_.orphan;
            return false;
        }
        levels_.remove(*sym, gone.side, gone.price, gone.shares);
        const PoolMap::Order fresh{
            itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff), gone.side};
        if (!orders_.insert(itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff),
                            fresh)) {
            return false;
        }
        put(*sym, fresh);
        return true;
    }

    void put(std::uint16_t sym, const PoolMap::Order& o) {
        const std::uint32_t was = levels_.at(sym, o.side, o.price);
        levels_.add(sym, o.side, o.price, o.shares);
        if (levels_.at(sym, o.side, o.price) == was) ++c_.untracked;
    }

    PoolMap orders_;
    PriceLevels levels_;
    Counters c_;
};

}
