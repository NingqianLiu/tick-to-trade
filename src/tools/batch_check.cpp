#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>

#include "io/seq_reader.hpp"
#include "itch/framing.hpp"
#include "itch/reader.hpp"
#include "itch/types.hpp"
#include "common/window.hpp"
#include "net/pack.hpp"

namespace {

constexpr std::uint32_t kMaxBatch = 131072;

struct Node {
    std::uint64_t oid = 0;
    std::uint32_t shares = 0;
    std::uint32_t price = 0;
    std::uint8_t side = 0;
    std::uint16_t sym = 0;
#ifdef ITCH_BATCH_STATS
    std::uint8_t born = 0, dying = 0, pending = 0;
    std::uint8_t born_by = 0;
    std::uint16_t depth = 0;
    std::uint8_t killed_by = 0;
#endif
};

class Table {
public:
    static constexpr std::uint32_t kNone = 0xffffffffu;
    [[nodiscard]] std::uint32_t find(std::uint64_t oid) const {
        const auto it = where_.find(oid);
        return it == where_.end() ? kNone : it->second;
    }
    std::uint32_t insert(std::uint64_t oid, const Node& n) {
        const auto it = where_.find(oid);
        if (it != where_.end()) { pool_[it->second] = n; pool_[it->second].oid = oid; return it->second; }
        std::uint32_t slot;
        if (!free_.empty()) { slot = free_.back(); free_.pop_back(); }
        else { slot = static_cast<std::uint32_t>(pool_.size()); pool_.push_back(Node{}); }
        pool_[slot] = n;
        pool_[slot].oid = oid;
        where_.emplace(oid, slot);
        return slot;
    }
    void erase_slot(std::uint32_t slot) {
        where_.erase(pool_[slot].oid);
        pool_[slot] = Node{};
        free_.push_back(slot);
    }
    [[nodiscard]] Node& at(std::uint32_t slot) { return pool_[slot]; }
    [[nodiscard]] const Node& at(std::uint32_t slot) const { return pool_[slot]; }
    [[nodiscard]] std::size_t size() const { return where_.size(); }
    [[nodiscard]] const std::unordered_map<std::uint64_t, std::uint32_t>& all() const {
        return where_;
    }

private:
    std::vector<Node> pool_;
    std::unordered_map<std::uint64_t, std::uint32_t> where_;
    std::vector<std::uint32_t> free_;
};

class Levels {
public:
    void move(std::uint16_t sym, std::uint8_t side, std::uint32_t price, std::int64_t delta) {
        if (delta == 0) return;
        ++moves;
        auto& tbl = tbl_[(std::size_t(sym) << 1) | side];
        auto it = tbl.find(price);
        if (it == tbl.end()) { if (delta > 0) tbl.emplace(price, delta); return; }
        it->second += delta;
        if (it->second <= 0) tbl.erase(it);
    }
    [[nodiscard]] bool same_as(const Levels& other) const {
        return tbl_ == other.tbl_;
    }
    [[nodiscard]] std::size_t count() const {
        std::size_t n = 0;
        for (const auto& t : tbl_) n += t.size();
        return n;
    }
    void first_diff(const Levels& other) const {
        for (std::size_t k = 0; k < tbl_.size(); ++k) {
            if (tbl_[k] == other.tbl_[k]) continue;
            std::printf("  first difference: security %zu, %s side\n", k >> 1, (k & 1) ? "ask" : "bid");
            auto a = tbl_[k].begin(), b = other.tbl_[k].begin();
            while (a != tbl_[k].end() || b != other.tbl_[k].end()) {
                if (a == tbl_[k].end()) { std::printf("    the fast one has an extra: price %u size %lld\n", b->first, (long long)b->second); ++b; continue; }
                if (b == other.tbl_[k].end()) { std::printf("    the slow one has an extra: price %u size %lld\n", a->first, (long long)a->second); ++a; continue; }
                if (a->first != b->first || a->second != b->second) {
                    std::printf("    price %u: slow %lld, fast %lld\n", a->first, (long long)a->second,
                                (long long)(a->first == b->first ? b->second : -1));
                }
                if (a->first <= b->first) ++a; else ++b;
            }
            return;
        }
    }

    std::uint64_t moves = 0;

private:
    std::vector<std::map<std::uint32_t, std::int64_t>> tbl_ =
        std::vector<std::map<std::uint32_t, std::int64_t>>(std::size_t{1} << 17);
};

struct UHit {
    const std::uint8_t* body = nullptr;
    std::uint32_t new_slot = Table::kNone;
    std::uint32_t old_slot = Table::kNone;
};

struct Batch {
    const std::uint8_t* add[kMaxBatch];
    std::uint32_t add_n = 0;
    UHit repl[kMaxBatch];
    std::uint32_t repl_n = 0;
    const std::uint8_t* cut[kMaxBatch];
    std::uint32_t cut_n = 0;
    const std::uint8_t* del[kMaxBatch];
    std::uint32_t del_n = 0;
    std::uint32_t born[kMaxBatch * 2];
    std::uint32_t born_n = 0;
    std::uint32_t dying[kMaxBatch * 2];
    std::uint32_t dying_n = 0;
    std::uint32_t chain[kMaxBatch];
    std::uint32_t chain_n = 0;
    std::uint32_t look[kMaxBatch];
    std::uint32_t keep[kMaxBatch];
    std::uint32_t kwant[kMaxBatch];
    std::uint32_t zero[kMaxBatch];
    void clear() { add_n = repl_n = cut_n = del_n = born_n = dying_n = chain_n = 0; }
};

struct Counters {
    std::uint64_t orphan = 0;
    std::uint64_t oversized = 0;
};

#ifdef ITCH_BATCH_STATS
struct Probe {
    std::uint64_t chains = 0;
    std::uint64_t born_and_died = 0;
    std::uint64_t cut_skipped = 0;
    std::uint64_t dead_u = 0;
    std::uint64_t died_by_d = 0, died_by_u = 0, died_by_e = 0;
    std::uint64_t d_hits_born = 0, d_total = 0;
    std::uint64_t u_total = 0, u_old_pre = 0, u_old_by_a = 0, u_old_by_u = 0;
    std::uint64_t depth_hist[17] = {};
    std::uint64_t max_depth = 0;
    std::uint64_t d_saves_a = 0, d_saves_u2 = 0, d_saves_ecx = 0;
    std::uint64_t u_saves_a = 0, u_saves_u2 = 0, u_saves_ecx = 0;
    std::uint64_t e_kills_a = 0, e_kills_u2 = 0;
    std::uint64_t n_add = 0, n_u = 0, n_del = 0, n_ecx = 0;
};
#endif

std::uint16_t sym_of(const std::uint8_t* b) { return itch::read_be<std::uint16_t>(b + itch::kLocateOff); }

void apply_one(Table* t, Levels* lv, Counters* c, const std::uint8_t* b) {
    const char type = static_cast<char>(b[itch::kTypeOff]);
    const std::uint16_t sym = sym_of(b);
    if (type == 'A' || type == 'F') {
        const std::uint64_t oid = itch::read_be<std::uint64_t>(b + itch::kAddRefOff);
        Node n;
        n.side = b[itch::kAddSideOff] == 'B' ? 0 : 1;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kAddSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kAddPriceOff);
        n.sym = sym;
        t->insert(oid, n);
        lv->move(sym, n.side, n.price, n.shares);
        return;
    }
    if (type == 'E' || type == 'C' || type == 'X') {
        const std::uint64_t oid = itch::read_be<std::uint64_t>(b + itch::kExecRefOff);
        const std::uint32_t want = itch::read_be<std::uint32_t>(b + itch::kExecSharesOff);
        const std::uint32_t slot = t->find(oid);
        if (slot == Table::kNone) { ++c->orphan; return; }
        Node& n = t->at(slot);
        if (want > n.shares) ++c->oversized;
        const std::uint32_t off = want < n.shares ? want : n.shares;
        n.shares -= off;
        lv->move(n.sym, n.side, n.price, -static_cast<std::int64_t>(off));
        if (n.shares == 0) t->erase_slot(slot);
        return;
    }
    if (type == 'D') {
        const std::uint64_t oid = itch::read_be<std::uint64_t>(b + itch::kAddRefOff);
        const std::uint32_t slot = t->find(oid);
        if (slot == Table::kNone) { ++c->orphan; return; }
        Node& n = t->at(slot);
        lv->move(n.sym, n.side, n.price, -static_cast<std::int64_t>(n.shares));
        t->erase_slot(slot);
        return;
    }
    if (type == 'U') {
        const std::uint64_t old_oid = itch::read_be<std::uint64_t>(b + itch::kReplaceOldRefOff);
        const std::uint32_t slot = t->find(old_oid);
        if (slot == Table::kNone) { ++c->orphan; return; }
        Node& o = t->at(slot);
        const std::uint8_t side = o.side;
        const std::uint16_t osym = o.sym;
        lv->move(osym, side, o.price, -static_cast<std::int64_t>(o.shares));
        t->erase_slot(slot);
        Node n;
        n.side = side;
        n.sym = osym;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kReplaceSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kReplacePriceOff);
        t->insert(itch::read_be<std::uint64_t>(b + itch::kReplaceNewRefOff), n);
        lv->move(osym, side, n.price, n.shares);
        return;
    }
}

#ifdef ITCH_BATCH_STATS
void apply_batch(Table* t, Levels* lv, Counters* c, Probe* pr, Batch* g, bool rev = false) {
    const auto pick = [rev](std::uint32_t k, std::uint32_t n) { return rev ? n - 1 - k : k; };
    for (std::uint32_t k = 0; k < g->add_n; ++k) {
        ++pr->n_add;
        const std::uint32_t i = pick(k, g->add_n);
        const std::uint8_t* b = g->add[i];
        Node n;
        n.side = b[itch::kAddSideOff] == 'B' ? 0 : 1;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kAddSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kAddPriceOff);
        n.sym = sym_of(b);
        n.born = 1;
        n.born_by = 1;
        n.depth = 0;
        const std::uint32_t slot = t->insert(itch::read_be<std::uint64_t>(b + itch::kAddRefOff), n);
        g->born[g->born_n++] = slot;
    }
    for (std::uint32_t i = 0; i < g->repl_n; ++i) {
        const std::uint8_t* b = g->repl[i].body;
        const std::uint32_t slot =
            t->find(itch::read_be<std::uint64_t>(b + itch::kReplaceOldRefOff));
        if (slot == Table::kNone) {
            ++c->orphan;
            ++pr->dead_u;
            continue;
        }
        g->repl[i].old_slot = slot;
        Node& o = t->at(slot);
        ++pr->u_total;
        ++pr->n_u;
        if (o.born_by == 0) ++pr->u_old_pre;
        else if (o.born_by == 1) ++pr->u_old_by_a;
        else ++pr->u_old_by_u;
        const std::uint16_t depth = static_cast<std::uint16_t>(
            (o.born_by == 2 ? o.depth : 0) + 1);
        ++pr->depth_hist[depth < 16 ? depth : 16];
        if (depth > pr->max_depth) pr->max_depth = depth;
        const std::uint8_t side = o.side;
        const std::uint16_t osym = o.sym;
        if (o.born == 0) lv->move(osym, side, o.price, -static_cast<std::int64_t>(o.shares));
        else {
            ++pr->died_by_u;
            o.killed_by = 2;
            if (o.born_by == 1) ++pr->u_saves_a; else ++pr->u_saves_u2;
        }
        o.dying = 1;
        g->dying[g->dying_n++] = slot;
        Node n;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kReplaceSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kReplacePriceOff);
        n.side = side;
        n.sym = osym;
        n.born = 1;
        n.born_by = 2;
        n.depth = depth;
        const std::uint32_t fresh =
            t->insert(itch::read_be<std::uint64_t>(b + itch::kReplaceNewRefOff), n);
        g->repl[i].new_slot = fresh;
        g->born[g->born_n++] = fresh;
    }
    for (std::uint32_t k = 0; k < g->del_n; ++k) {
        const std::uint32_t i = pick(k, g->del_n);
        ++pr->d_total;
        const std::uint32_t slot =
            t->find(itch::read_be<std::uint64_t>(g->del[i] + itch::kAddRefOff));
        if (slot == Table::kNone) { ++c->orphan; continue; }
        Node& n = t->at(slot);
        if (n.dying != 0) continue;
        n.dying = 1;
        g->dying[g->dying_n++] = slot;
        if (n.born == 0) lv->move(n.sym, n.side, n.price, -static_cast<std::int64_t>(n.shares));
        else {
            ++pr->died_by_d;
            ++pr->d_hits_born;
            n.killed_by = 1;
            if (n.born_by == 1) ++pr->d_saves_a; else ++pr->d_saves_u2;
        }
    }
    for (std::uint32_t k = 0; k < g->cut_n; ++k) {
        const std::uint32_t i = pick(k, g->cut_n);
        ++pr->n_ecx;
        const std::uint8_t* b = g->cut[i];
        const std::uint32_t slot = t->find(itch::read_be<std::uint64_t>(b + itch::kExecRefOff));
        if (slot == Table::kNone) { ++c->orphan; continue; }
        Node& n = t->at(slot);
        if (n.killed_by == 1) ++pr->d_saves_ecx;
        else if (n.killed_by == 2) ++pr->u_saves_ecx;
        const std::uint32_t want = itch::read_be<std::uint32_t>(b + itch::kExecSharesOff);
        if (n.dying != 0) {
            if (want > n.shares) ++c->oversized;
            ++pr->cut_skipped;
            continue;
        }
        if (want > n.shares) ++c->oversized;
        const std::uint32_t off = want < n.shares ? want : n.shares;
        n.shares -= off;
        if (n.born == 0) lv->move(n.sym, n.side, n.price, -static_cast<std::int64_t>(off));
        if (n.shares == 0) {
            n.dying = 1;
            g->dying[g->dying_n++] = slot;
            if (n.born != 0) {
                ++pr->died_by_e;
                n.killed_by = 3;
                if (n.born_by == 1) ++pr->e_kills_a; else ++pr->e_kills_u2;
            }
        }
    }
    for (std::uint32_t q = 0; q < g->born_n; ++q) {
        Node& n = t->at(g->born[q]);
        if (n.dying == 0) lv->move(n.sym, n.side, n.price, static_cast<std::int64_t>(n.shares));
        else ++pr->born_and_died;
        n.born = 0;
        n.pending = 0;
        n.born_by = 0;
        n.depth = 0;
        n.killed_by = 0;
    }
    for (std::uint32_t q = 0; q < g->dying_n; ++q) {
        if (t->at(g->dying[q]).oid == 0 && t->at(g->dying[q]).shares == 0 &&
            t->at(g->dying[q]).dying == 0) {
            continue;
        }
        t->erase_slot(g->dying[q]);
    }
    g->clear();
}
#endif

void put(std::vector<std::uint8_t>* keep, std::vector<std::uint32_t>* at, char type,
         std::uint16_t sym, std::uint64_t oid, char buy_sell, std::uint32_t shares,
         std::uint32_t price, std::uint64_t new_oid) {
    at->push_back(static_cast<std::uint32_t>(keep->size()));
    const std::size_t len = itch::kBodyLen[static_cast<unsigned char>(type)];
    const std::size_t base = keep->size();
    keep->resize(base + len, 0);
    std::uint8_t* b = keep->data() + base;
    b[itch::kTypeOff] = static_cast<std::uint8_t>(type);
    b[itch::kLocateOff] = static_cast<std::uint8_t>(sym >> 8);
    b[itch::kLocateOff + 1] = static_cast<std::uint8_t>(sym & 0xff);
    const auto put64 = [](std::uint8_t* p, std::uint64_t v) {
        for (int k = 0; k < 8; ++k) p[k] = static_cast<std::uint8_t>(v >> (56 - 8 * k));
    };
    const auto put32 = [](std::uint8_t* p, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) p[k] = static_cast<std::uint8_t>(v >> (24 - 8 * k));
    };
    if (type == 'A' || type == 'F') {
        put64(b + itch::kAddRefOff, oid);
        b[itch::kAddSideOff] = static_cast<std::uint8_t>(buy_sell);
        put32(b + itch::kAddSharesOff, shares);
        put32(b + itch::kAddPriceOff, price);
    } else if (type == 'E' || type == 'C' || type == 'X') {
        put64(b + itch::kExecRefOff, oid);
        put32(b + itch::kExecSharesOff, shares);
    } else if (type == 'D') {
        put64(b + itch::kAddRefOff, oid);
    } else if (type == 'U') {
        put64(b + itch::kReplaceOldRefOff, oid);
        put64(b + itch::kReplaceNewRefOff, new_oid);
        put32(b + itch::kReplaceSharesOff, shares);
        put32(b + itch::kReplacePriceOff, price);
    }
}

void apply_batch_plain(Table* t, Levels* lv, Counters* c, Batch* g) {
    (void)c;
    for (std::uint32_t i = 0; i < g->add_n; ++i) {
        const std::uint8_t* b = g->add[i];
        Node n;
        n.side = b[itch::kAddSideOff] == 'B' ? 0 : 1;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kAddSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kAddPriceOff);
        n.sym = sym_of(b);
        t->insert(itch::read_be<std::uint64_t>(b + itch::kAddRefOff), n);
        lv->move(n.sym, n.side, n.price, static_cast<std::int64_t>(n.shares));
    }
    for (std::uint32_t i = 0; i < g->repl_n; ++i) {
        const std::uint8_t* b = g->repl[i].body;
        Node n;
        n.shares = itch::read_be<std::uint32_t>(b + itch::kReplaceSharesOff);
        n.price = itch::read_be<std::uint32_t>(b + itch::kReplacePriceOff);
        n.sym = sym_of(b);
        n.side = 0;
        g->repl[i].new_slot =
            t->insert(itch::read_be<std::uint64_t>(b + itch::kReplaceNewRefOff), n);
    }
    std::uint32_t missing = 0;
    for (std::uint32_t i = 0; i < g->repl_n; ++i) {
        const std::uint32_t slot =
            t->find(itch::read_be<std::uint64_t>(g->repl[i].body + itch::kReplaceOldRefOff));
        g->repl[i].old_slot = slot;
        missing += (slot == Table::kNone) ? 1u : 0u;
    }
    if (missing == 0) {
        for (std::uint32_t i = 0; i < g->repl_n; ++i) {
            const Node& o = t->at(g->repl[i].old_slot);
            Node& fresh = t->at(g->repl[i].new_slot);
            fresh.side = o.side;
            fresh.sym = o.sym;
        }
        for (std::uint32_t i = 0; i < g->repl_n; ++i) {
            const Node& o = t->at(g->repl[i].old_slot);
            lv->move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            t->erase_slot(g->repl[i].old_slot);
            const Node& fresh = t->at(g->repl[i].new_slot);
            lv->move(fresh.sym, fresh.side, fresh.price, static_cast<std::int64_t>(fresh.shares));
        }
    } else {
        for (std::uint32_t i = 0; i < g->repl_n; ++i) {
            if (g->repl[i].old_slot == Table::kNone) continue;
            const Node& o = t->at(g->repl[i].old_slot);
            Node& fresh = t->at(g->repl[i].new_slot);
            fresh.side = o.side;
            fresh.sym = o.sym;
        }
        for (std::uint32_t i = 0; i < g->repl_n; ++i) {
            if (g->repl[i].old_slot == Table::kNone) {
                t->erase_slot(g->repl[i].new_slot);
                continue;
            }
            const Node& o = t->at(g->repl[i].old_slot);
            lv->move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            t->erase_slot(g->repl[i].old_slot);
            const Node& fresh = t->at(g->repl[i].new_slot);
            lv->move(fresh.sym, fresh.side, fresh.price, static_cast<std::int64_t>(fresh.shares));
        }
    }
    {
        for (std::uint32_t i = 0; i < g->del_n; ++i) {
            g->look[i] = t->find(itch::read_be<std::uint64_t>(g->del[i] + itch::kAddRefOff));
        }
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < g->del_n; ++i) {
            g->keep[n] = g->look[i];
            n += (g->look[i] != Table::kNone) ? 1u : 0u;
        }
        for (std::uint32_t j = 0; j < n; ++j) {
            const Node& nd = t->at(g->keep[j]);
            lv->move(nd.sym, nd.side, nd.price, -static_cast<std::int64_t>(nd.shares));
            t->erase_slot(g->keep[j]);
        }
    }
    {
        for (std::uint32_t i = 0; i < g->cut_n; ++i) {
            const std::uint8_t* b = g->cut[i];
            g->look[i] = t->find(itch::read_be<std::uint64_t>(b + itch::kExecRefOff));
            g->kwant[i] = itch::read_be<std::uint32_t>(b + itch::kExecSharesOff);
        }
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < g->cut_n; ++i) {
            g->keep[n] = g->look[i];
            g->kwant[n] = g->kwant[i];
            n += (g->look[i] != Table::kNone) ? 1u : 0u;
        }
        std::uint32_t z = 0;
        for (std::uint32_t j = 0; j < n; ++j) {
            Node& nd = t->at(g->keep[j]);
            const std::uint32_t off = g->kwant[j] < nd.shares ? g->kwant[j] : nd.shares;
            nd.shares -= off;
            lv->move(nd.sym, nd.side, nd.price, -static_cast<std::int64_t>(off));
            g->zero[z] = g->keep[j];
            z += (nd.shares == 0) ? 1u : 0u;
        }
        for (std::uint32_t k = 0; k < z; ++k) t->erase_slot(g->zero[k]);
    }
    g->clear();
}

bool same_table(const Table& a, const Table& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [oid, slot] : a.all()) {
        const std::uint32_t s2 = b.find(oid);
        if (s2 == Table::kNone) return false;
        const Node& x = a.at(slot);
        const Node& y = b.at(s2);
        if (x.shares != y.shares || x.price != y.price || x.side != y.side || x.sym != y.sym) {
            return false;
        }
    }
    return true;
}

}

int selftest(bool rev, bool plain) {
    struct Case { const char* name; void (*build)(std::vector<std::uint8_t>*, std::vector<std::uint32_t>*); };
    static const Case cases[] = {
        {"two U in a chain", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'A', 7, 100, 'B', 500, 1000, 0);
            put(k, a, 'U', 7, 100, 0, 400, 1010, 200);
            put(k, a, 'U', 7, 200, 0, 300, 1020, 300);
        }},
        {"three U in a chain", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'U', 7, 900, 0, 400, 1010, 901);
            put(k, a, 'U', 7, 901, 0, 300, 1020, 902);
            put(k, a, 'U', 7, 902, 0, 200, 1030, 903);
        }},
        {"an order made by U, deleted in the same batch", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'A', 7, 110, 'S', 500, 2000, 0);
            put(k, a, 'U', 7, 110, 0, 400, 2010, 210);
            put(k, a, 'D', 7, 210, 0, 0, 0, 0);
        }},
        {"an order made by U, cut by E, then replaced", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'A', 7, 120, 'B', 900, 3000, 0);
            put(k, a, 'U', 7, 120, 0, 800, 3010, 220);
            put(k, a, 'E', 7, 220, 0, 100, 0, 0);
            put(k, a, 'U', 7, 220, 0, 500, 3020, 320);
        }},
        {"an order added by A, cut to zero in the same batch", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'A', 7, 130, 'S', 100, 4000, 0);
            put(k, a, 'E', 7, 130, 0, 60, 0, 0);
            put(k, a, 'E', 7, 130, 0, 40, 0, 0);
        }},
        {"a U whose old order is not held", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'U', 7, 77777, 0, 100, 5000, 88888);
            put(k, a, 'A', 7, 140, 'B', 200, 5010, 0);
        }},
        {"an old order cut by E and then deleted", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'E', 7, 910, 0, 100, 0, 0);
            put(k, a, 'D', 7, 910, 0, 0, 0, 0);
        }},
        {"two securities interleaved", [](std::vector<std::uint8_t>* k, std::vector<std::uint32_t>* a) {
            put(k, a, 'A', 7, 150, 'B', 300, 6000, 0);
            put(k, a, 'A', 8, 151, 'S', 400, 6000, 0);
            put(k, a, 'U', 7, 150, 0, 250, 6010, 250);
            put(k, a, 'U', 8, 151, 0, 350, 6010, 251);
            put(k, a, 'D', 8, 251, 0, 0, 0, 0);
        }},
    };
    int bad = 0;
    for (const Case& cs : cases) {
        Table slow_t, fast_t;
        Levels slow_l, fast_l;
        Counters slow_c, fast_c;
#ifdef ITCH_BATCH_STATS
        Probe pr;
#endif
        std::vector<Batch> hold(1);
        Batch& g = hold[0];
        std::vector<std::uint8_t> keep;
        std::vector<std::uint32_t> at;
        std::vector<std::uint8_t> pre;
        std::vector<std::uint32_t> pre_at;
        put(&pre, &pre_at, 'A', 7, 900, 'B', 700, 900, 0);
        put(&pre, &pre_at, 'A', 7, 910, 'S', 700, 910, 0);
        for (std::uint32_t off : pre_at) {
            apply_one(&slow_t, &slow_l, &slow_c, pre.data() + off);
            apply_one(&fast_t, &fast_l, &fast_c, pre.data() + off);
        }
        cs.build(&keep, &at);
        for (std::uint32_t off : at) apply_one(&slow_t, &slow_l, &slow_c, keep.data() + off);
        for (std::uint32_t off : at) {
            const std::uint8_t* b = keep.data() + off;
            switch (static_cast<char>(b[itch::kTypeOff])) {
                case 'A': case 'F': g.add[g.add_n++] = b; break;
                case 'U': g.repl[g.repl_n].body = b;
                          g.repl[g.repl_n].new_slot = Table::kNone;
                          g.repl[g.repl_n].old_slot = Table::kNone;
                          ++g.repl_n; break;
                case 'E': case 'C': case 'X': g.cut[g.cut_n++] = b; break;
                case 'D': g.del[g.del_n++] = b; break;
                default: break;
            }
        }
#ifdef ITCH_BATCH_STATS
        if (plain) apply_batch_plain(&fast_t, &fast_l, &fast_c, &g);
        else apply_batch(&fast_t, &fast_l, &fast_c, &pr, &g, rev);
#else
        (void)rev;
        apply_batch_plain(&fast_t, &fast_l, &fast_c, &g);
#endif
        const bool ok = same_table(slow_t, fast_t) && slow_l.same_as(fast_l) &&
                        (plain || (slow_c.orphan == fast_c.orphan &&
                                   slow_c.oversized == fast_c.oversized));
        std::printf("  %-28s %s   live slow %zu fast %zu, levels slow %zu fast %zu, chains %llu\n",
                    cs.name, ok ? "ok" : "BAD", slow_t.size(), fast_t.size(),
                    slow_l.count(), fast_l.count(),
#ifdef ITCH_BATCH_STATS
                    (unsigned long long)pr.chains
#else
                    0ull
#endif
                    );
        if (!ok) { ++bad; slow_l.first_diff(fast_l); }
    }
    return bad;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0) {
        int bad = 0;
#ifdef ITCH_BATCH_STATS
        std::printf("the version with the tricks, forwards:\n");
        bad += selftest(false, false);
        std::printf("the version with the tricks, backwards (the passes do not depend on order, and it walks the chain path):\n");
        bad += selftest(true, false);
#endif
        std::printf("the plain version, buckets only:\n");
        bad += selftest(false, true);
        std::printf(bad == 0 ? "all identical\n" : "%d did not match\n", bad);
        return bad == 0 ? 0 : 1;
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: batch_check ITCH_FILE [--stop N] [--batch N]\n"
                             "       batch_check --selftest\n");
        return 2;
    }
    std::uint64_t stop_after = 0;
    std::uint32_t batch_size = 256;
    bool rev = false;
    bool plain = false;
    std::uint64_t from_sec = 0;
    std::uint64_t to_sec = 0;
    std::uint32_t packets_per_batch = 0;
    std::uint64_t check_every = 1;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stop") == 0 && i + 1 < argc) {
            stop_after = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            batch_size = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (batch_size == 0 || batch_size > kMaxBatch) { std::fprintf(stderr, "bad --batch\n"); return 2; }
        } else if (std::strcmp(argv[i], "--check-every") == 0 && i + 1 < argc) {
            check_every = std::strtoull(argv[++i], nullptr, 10);
            if (check_every == 0) check_every = 1;
        } else if (std::strcmp(argv[i], "--packets") == 0 && i + 1 < argc) {
            packets_per_batch = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (packets_per_batch == 0) { std::fprintf(stderr, "bad --packets\n"); return 2; }
        } else if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            int hh = 0, mm = 0;
            if (std::sscanf(argv[++i], "%d:%d", &hh, &mm) != 2) {
                std::fprintf(stderr, "bad --from, want HH:MM\n"); return 2;
            }
            from_sec = static_cast<std::uint64_t>(hh) * 3600 + static_cast<std::uint64_t>(mm) * 60;
        } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            int hh = 0, mm = 0;
            if (std::sscanf(argv[++i], "%d:%d", &hh, &mm) != 2) {
                std::fprintf(stderr, "bad --to, want HH:MM\n"); return 2;
            }
            to_sec = static_cast<std::uint64_t>(hh) * 3600 + static_cast<std::uint64_t>(mm) * 60;
        } else if (std::strcmp(argv[i], "--plain") == 0) {
            plain = true;
        } else if (std::strcmp(argv[i], "--reverse") == 0) {
            rev = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    io::SeqReader reader(argv[1], 32u << 20);
    if (!reader.ok()) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }

    Table slow_t, fast_t;
    Levels slow_l, fast_l;
    Counters slow_c, fast_c;
#ifdef ITCH_BATCH_STATS
    Probe pr;
#endif
    std::vector<Batch> hold(1);
    Batch& g = hold[0];

    std::vector<std::uint8_t> keep;
    keep.reserve(std::size_t(kMaxBatch) * 64);
    std::vector<std::uint32_t> at;
    at.reserve(kMaxBatch);

    std::uint64_t messages = 0, touched = 0, batches = 0, mismatch = 0, packets = 0;
    bool stopped = false;
    bool counting = from_sec == 0;
    const win::Params wp = win::params_from_env();
    win::Tracker tracker(wp);
    pkt::Packing packing;
    std::uint32_t in_batch = 0;

    const auto run_batch = [&]() {
        if (at.empty()) return;
        ++batches;
        for (std::uint32_t off : at) apply_one(&slow_t, &slow_l, &slow_c, keep.data() + off);
        for (std::uint32_t off : at) {
            const std::uint8_t* b = keep.data() + off;
            switch (static_cast<char>(b[itch::kTypeOff])) {
                case 'A': case 'F': g.add[g.add_n++] = b; break;
                case 'U': g.repl[g.repl_n].body = b;
                          g.repl[g.repl_n].new_slot = Table::kNone;
                          g.repl[g.repl_n].old_slot = Table::kNone;
                          ++g.repl_n; break;
                case 'E': case 'C': case 'X': g.cut[g.cut_n++] = b; break;
                case 'D': g.del[g.del_n++] = b; break;
                default: break;
            }
        }
#ifdef ITCH_BATCH_STATS
        if (plain) apply_batch_plain(&fast_t, &fast_l, &fast_c, &g);
        else apply_batch(&fast_t, &fast_l, &fast_c, &pr, &g, rev);
#else
        (void)plain; (void)rev;
        apply_batch_plain(&fast_t, &fast_l, &fast_c, &g);
#endif
        keep.clear();
        at.clear();
        if (!counting) return;
        if (batches % check_every != 0) return;
        if (!same_table(slow_t, fast_t) || !slow_l.same_as(fast_l) ||
            slow_c.orphan != fast_c.orphan || slow_c.oversized != fast_c.oversized) {
            ++mismatch;
            if (mismatch == 1) {
                std::printf("batch %llu did not match\n", (unsigned long long)batches);
                std::printf("  live orders slow %zu fast %zu\n", slow_t.size(), fast_t.size());
                std::printf("  price levels slow %zu fast %zu\n", slow_l.count(), fast_l.count());
                std::printf("  unknown orders slow %llu fast %llu\n",
                            (unsigned long long)slow_c.orphan, (unsigned long long)fast_c.orphan);
                slow_l.first_diff(fast_l);
            }
            stopped = true;
        }
    };

    while (!stopped && reader.fill()) {
        const auto r = itch::for_each_message(
            reader.data(), reader.size(), [&](const itch::Message& m) {
                ++messages;
                if (counting && stop_after != 0 && messages >= stop_after) { stopped = true; return false; }
                if (packets_per_batch != 0) {
                    win::note_session(m, &tracker);
                    const win::Phase ph = tracker.advance(m.timestamp());
                    const std::size_t rec = m.len + itch::kLenPrefix;
                    if (packing.should_close(m.timestamp(), rec, ph)) {
                        packing.close();
                        ++packets;
                        if (++in_batch >= packets_per_batch) { run_batch(); in_batch = 0; }
                    }
                    packing.add(m.timestamp(), rec, ph);
                }
                if (to_sec != 0 && m.timestamp() / 1000000000ull >= to_sec) {
                    stopped = true;
                    return false;
                }
                if (!counting && m.timestamp() / 1000000000ull >= from_sec) {
                    counting = true;
                    touched = 0; batches = 0; packets = 0;
                    slow_l.moves = 0; fast_l.moves = 0;
#ifdef ITCH_BATCH_STATS
                    pr = Probe{};
#endif
                    messages = 0;
                }
                const char type = m.type();
                if (type != 'A' && type != 'F' && type != 'E' && type != 'C' &&
                    type != 'X' && type != 'D' && type != 'U') {
                    return true;
                }
                if (counting) ++touched;
                at.push_back(static_cast<std::uint32_t>(keep.size()));
                keep.insert(keep.end(), m.body, m.body + m.len);
                if (packets_per_batch == 0 && at.size() >= batch_size) run_batch();
                if (at.size() >= kMaxBatch) { run_batch(); in_batch = 0; }
                return !stopped;
            });
        reader.consume(r.consumed);
        if (r.stop == itch::FrameStop::kZeroLength) { std::fprintf(stderr, "zero length record\n"); return 1; }
    }
    if (!stopped) run_batch();
    if (mismatch == 0 && (!same_table(slow_t, fast_t) || !slow_l.same_as(fast_l) ||
                          slow_c.orphan != fast_c.orphan ||
                          slow_c.oversized != fast_c.oversized)) {
        ++mismatch;
        std::printf("the two books differ at the end of the run\n");
        slow_l.first_diff(fast_l);
    }

    std::printf("messages       %llu\n", (unsigned long long)messages);
    std::printf("touched the book %llu\n", (unsigned long long)touched);
    if (packets_per_batch != 0) {
        std::printf("packets        %llu (%u packets per batch)\n",
                    (unsigned long long)packets, packets_per_batch);
    }
    std::printf("batches        %llu (at most %u messages each)\n",
                (unsigned long long)batches, packets_per_batch != 0 ? kMaxBatch : batch_size);
    std::printf("one at a time  %llu level touches, %.4f per ITCH message\n",
                (unsigned long long)slow_l.moves,
                touched ? double(slow_l.moves) / double(touched) : 0.0);
    std::printf("bucketed pass  %llu level touches, %.4f per ITCH message\n",
                (unsigned long long)fast_l.moves,
                touched ? double(fast_l.moves) / double(touched) : 0.0);
    std::printf("saved          %.1f%%\n",
                slow_l.moves ? 100.0 * (1.0 - double(fast_l.moves) / double(slow_l.moves)) : 0.0);
    std::printf("live orders    slow %zu   fast %zu\n", slow_t.size(), fast_t.size());
    std::printf("levels in use  slow %zu   fast %zu\n", slow_l.count(), fast_l.count());
    std::printf("unknown orders slow %llu   fast %llu\n",
                (unsigned long long)slow_c.orphan, (unsigned long long)fast_c.orphan);
    std::printf("oversized      slow %llu   fast %llu\n",
                (unsigned long long)slow_c.oversized, (unsigned long long)fast_c.oversized);
#ifdef ITCH_BATCH_STATS
    std::printf("\npaths only the fast one takes, and how often:\n");
    std::printf("  chain, a U whose old order came from another U in the batch   %llu\n", (unsigned long long)pr.chains);
    std::printf("  orders born and dead inside the same batch                    %llu\n", (unsigned long long)pr.born_and_died);
    std::printf("  E/C/X skipped because the target will be deleted anyway       %llu\n", (unsigned long long)pr.cut_skipped);
    std::printf("  U dropped because its old order is not held                   %llu\n", (unsigned long long)pr.dead_u);
    const double bd = double(pr.born_and_died ? pr.born_and_died : 1);
    std::printf("\nwhat killed the orders born and dead in the same batch (%llu of them):\n",
                (unsigned long long)pr.born_and_died);
    std::printf("  killed by D          %10llu (%.1f%%)   only these can be saved by running D first\n",
                (unsigned long long)pr.died_by_d, 100.0 * double(pr.died_by_d) / bd);
    std::printf("  killed by U          %10llu (%.1f%%)   cannot be saved, U takes the side from the old order\n",
                (unsigned long long)pr.died_by_u, 100.0 * double(pr.died_by_u) / bd);
    std::printf("  cut to zero by E/C/X %7llu (%.1f%%)   cannot be saved, the cut depends on what is left\n",
                (unsigned long long)pr.died_by_e, 100.0 * double(pr.died_by_e) / bd);
    std::printf("\nwhere the old order of a replace comes from (%llu U messages):\n", (unsigned long long)pr.u_total);
    {
        const double ut = double(pr.u_total ? pr.u_total : 1);
        std::printf("  already there before  %10llu (%.2f%%)   independent of the other U here\n",
                    (unsigned long long)pr.u_old_pre, 100.0 * double(pr.u_old_pre) / ut);
        std::printf("  added by A/F here     %10llu (%.2f%%)   the side is in that add message\n",
                    (unsigned long long)pr.u_old_by_a, 100.0 * double(pr.u_old_by_a) / ut);
        std::printf("  made by a U here      %10llu (%.2f%%)   <- this is the chain\n",
                    (unsigned long long)pr.u_old_by_u, 100.0 * double(pr.u_old_by_u) / ut);
        std::printf("  longest chain in a batch  %llu links\n", (unsigned long long)pr.max_depth);
        std::printf("  chain length, link to how many U:");
        for (int k = 1; k <= 16; ++k) {
            if (pr.depth_hist[k] == 0) continue;
            std::printf(" %d:%llu", k, (unsigned long long)pr.depth_hist[k]);
        }
        std::printf("\n");
    }
    {
        const double tt = double(touched ? touched : 1);
        std::printf("\nif the deletes run first, how many messages can be skipped entirely:\n");
        std::printf("  in this stretch: touching the book %llu | A/F %llu | U %llu | D %llu | E/C/X %llu\n",
                    (unsigned long long)touched, (unsigned long long)pr.n_add,
                    (unsigned long long)pr.n_u, (unsigned long long)pr.d_total,
                    (unsigned long long)pr.n_ecx);
        const std::uint64_t ds = pr.d_saves_a + pr.d_saves_u2 + pr.d_saves_ecx;
        const std::uint64_t us = pr.u_saves_a + pr.u_saves_u2 + pr.u_saves_ecx;
        std::printf("  D first         saves A/F %llu + the add half of U %llu + E/C/X %llu = %llu messages (%.2f%%)\n",
                    (unsigned long long)pr.d_saves_a, (unsigned long long)pr.d_saves_u2,
                    (unsigned long long)pr.d_saves_ecx, (unsigned long long)ds, 100.0 * double(ds) / tt);
        std::printf("  U deletes first saves A/F %llu + the add half of U %llu + E/C/X %llu = %llu messages (%.2f%%)\n",
                    (unsigned long long)pr.u_saves_a, (unsigned long long)pr.u_saves_u2,
                    (unsigned long long)pr.u_saves_ecx, (unsigned long long)us, 100.0 * double(us) / tt);
        std::printf("  neither saves these, cut to zero by E/C/X: A/F %llu + the add half of U %llu\n",
                    (unsigned long long)pr.e_kills_a, (unsigned long long)pr.e_kills_u2);
    }
    std::printf("%llu D messages, %llu of them target an order built in the same batch (%.1f%%)\n",
                (unsigned long long)pr.d_total, (unsigned long long)pr.d_hits_born,
                pr.d_total ? 100.0 * double(pr.d_hits_born) / double(pr.d_total) : 0.0);
    if (pr.chains == 0 || pr.born_and_died == 0 || pr.cut_skipped == 0) {
        std::printf("one of the paths above was never taken, so this run does not prove the algorithm\n");
    }
#endif
    if (mismatch != 0) { std::printf("mismatch\n"); return 1; }
    std::printf("every batch matched exactly\n");
    return 0;
}
