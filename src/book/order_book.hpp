#pragma once

#include <cstdint>
#include <vector>

#include "book/inline_map.hpp"
#include "book/order_map.hpp"
#include "book/pool_map.hpp"
#include "book/price_levels.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"

namespace book {

using OrderTable = PoolMap;

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
    [[nodiscard]] std::uint8_t last_side() const noexcept { return last_side_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return orders_.capacity(); }
    [[nodiscard]] const Counters& counters() const noexcept { return c_; }
    [[nodiscard]] const PriceLevels& levels() const noexcept { return levels_; }

    static constexpr std::uint32_t kNoSlot = OrderTable::kNoSlot;

    std::uint32_t insert_at(std::uint64_t oid, const OrderTable::Order& o) {
        return orders_.insert_at(oid, o);
    }
    [[nodiscard]] std::uint32_t find_slot(std::uint64_t oid) const {
        return orders_.find_slot(oid);
    }
    [[nodiscard]] OrderTable::Order at(std::uint32_t slot) const { return orders_.at(slot); }
    void set_shares_at(std::uint32_t slot, std::uint32_t shares) {
        orders_.set_shares_at(slot, shares);
    }
    void set_side_sym_at(std::uint32_t slot, std::uint8_t side, std::uint16_t sym) {
        orders_.set_side_sym_at(slot, side, sym);
    }
    void erase_at(std::uint32_t slot) { orders_.erase_at(slot); }

    void level_move(std::uint16_t sym, std::uint8_t side, std::uint32_t price,
                    std::int64_t delta) {
        if (delta > 0) {
            levels_.add(sym, side, price, static_cast<std::uint32_t>(delta));
        } else {
            levels_.remove(sym, side, price, static_cast<std::uint32_t>(-delta));
        }
    }

    void ask_for(const std::uint64_t* oids, std::size_t n) const {
        orders_.ask_for_all(oids, n);
    }

private:
    static std::uint8_t side_of(const itch::Message& m) {
        return m.body[itch::kAddSideOff] == 'B' ? PriceLevels::kBuy : PriceLevels::kSell;
    }

    bool add(const itch::Message& m) {
        const std::uint64_t oid = itch::read_be<std::uint64_t>(m.body + itch::kAddRefOff);
        const OrderTable::Order o{
            itch::read_be<std::uint32_t>(m.body + itch::kAddSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kAddPriceOff), side_of(m)};
        ++c_.added;
        OrderTable::Order old{};
        if (orders_.find(oid, &old)) {
            ++c_.duplicate;
            last_side_ = old.side;
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
        OrderTable::Order before{};
        if (!orders_.reduce(oid, want, &before)) {
            ++c_.orphan;
            return false;
        }
        if (want > before.shares) ++c_.oversized;
        const std::uint32_t took = want < before.shares ? want : before.shares;
        last_side_ = before.side;
        levels_.remove(m.stock_locate(), before.side, before.price, took);
        return true;
    }

    bool remove(const itch::Message& m) {
        ++c_.deleted;
        OrderTable::Order gone{};
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kDeleteRefOff),
                           &gone)) {
            ++c_.orphan;
            return false;
        }
        last_side_ = gone.side;
        levels_.remove(m.stock_locate(), gone.side, gone.price, gone.shares);
        return true;
    }

    bool replace(const itch::Message& m, std::uint16_t* sym) {
        ++c_.replaced;
        OrderTable::Order gone{};
        if (!orders_.erase(itch::read_be<std::uint64_t>(m.body + itch::kReplaceOldRefOff),
                           &gone)) {
            ++c_.orphan;
            return false;
        }
        last_side_ = gone.side;
        levels_.remove(*sym, gone.side, gone.price, gone.shares);
        const OrderTable::Order fresh{
            itch::read_be<std::uint32_t>(m.body + itch::kReplaceSharesOff),
            itch::read_be<std::uint32_t>(m.body + itch::kReplacePriceOff), gone.side};
        if (!orders_.insert(itch::read_be<std::uint64_t>(m.body + itch::kReplaceNewRefOff),
                            fresh)) {
            return false;
        }
        put(*sym, fresh);
        return true;
    }

    void put(std::uint16_t sym, const OrderTable::Order& o) {
        const std::uint32_t was = levels_.at(sym, o.side, o.price);
        last_side_ = o.side;
        levels_.add(sym, o.side, o.price, o.shares);
        if (levels_.at(sym, o.side, o.price) == was) ++c_.untracked;
    }

    std::uint8_t last_side_ = 0;

    OrderTable orders_;
    PriceLevels levels_;
    Counters c_;
};

}
