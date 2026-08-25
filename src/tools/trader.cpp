#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <onload/extensions.h>

#include <etherfabric/ef_vi.h>
#include <etherfabric/vi.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book/imbalance.hpp"
#include "book/order_book.hpp"
#include "common/samples.hpp"
#include "common/settings.hpp"
#include "common/tsc.hpp"
#include "common/window.hpp"
#include "itch/framing.hpp"
#include "net/ef.hpp"
#include "net/eth.hpp"
#include "net/mold.hpp"
#include "common/hist.hpp"
#include "common/huge.hpp"
#include "net/nic_stats.hpp"
#include "net/mintcp.hpp"
#include "net/ouch.hpp"

namespace {

struct Options {
    const char* intf = "enp129s0f1";
    const char* reference = "results/prev_close_061226.csv";
    const char* symbols = nullptr;
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    std::uint16_t dst_port = 26477;
    std::uint16_t dst_port_b = 0;
    int cpu_base = 4;
    std::uint32_t threshold = 75;
    bool group = true;
    bool split_ab = false;
    std::uint64_t idle_ms = 2000;
    const char* order_ip = nullptr;
    std::uint16_t order_port = 45100;
    const char* order_local = "10.9.9.2";
    const char* stat = nullptr;
    bool segments = false;
    const char* out = nullptr;
    bool batch_packets = false;
    bool coalesce = true;
    bool lock_memory = false;
};

constexpr std::size_t kOrderSlots = 64;
constexpr int kHeaderRoom = 128;
constexpr std::size_t kMaxPerFrame = 28;
static_assert(kHeaderRoom + kMaxPerFrame * 50 <= cfg::kOrderSlotBytes,
              "a full message of orders must fit one transmit slot");
static_assert(mintcp::kHeaderLen + kMaxPerFrame * 50 <= cfg::kOrderSlotBytes,
              "our own header and a full message must fit one transmit slot");

constexpr std::size_t kMaxPollEvents = 256;

constexpr std::size_t kMaxMsgsPerPacket =
    (cfg::kRxSlotBytes - cfg::kRxPrefixBytes - eth::kHeaderBytes - mold::kHeaderLen) / 14;
constexpr std::size_t kMaxHits = kMaxPollEvents * kMaxMsgsPerPacket;

struct Shard {
    Shard(std::size_t orders, std::size_t level_words, std::uint32_t pct,
          const win::Params& wp)
        : book(orders, level_words), signal(pct), phase(wp), symbols(65536 * 8, ' ') {}

    book::OrderBook book;
    book::Imbalance signal;
    win::Tracker phase;
    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    std::uint64_t applied_window = 0;
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0;
    std::uint64_t expect_seq = 0;
    bool started = false;

    ef::Vi tx;
    ef::Frames txbuf;
    std::size_t next_slot = 0;
    std::uint32_t user_ref = 1;
    std::uint64_t sent = 0, refused = 0, stamped = 0, reaped = 0, no_slot = 0;
    std::uint64_t wnd_block = 0;
    std::uint32_t wnd_min = 0xffffffffu, wnd_max = 0;
    std::uint64_t frames = 0;
    ef_vi_tx_warm_state warm_state{};
    std::uint64_t since_send = 0, warmed = 0;
    struct Order {
        std::uint64_t rx = 0;
        std::uint64_t poll = 0;
        std::uint64_t in = 0;
        std::uint64_t s1 = 0, s2 = 0, s3 = 0, out = 0;
        std::uint64_t body = 0;
        std::uint64_t parse = 0, book = 0;
        std::uint16_t before = 0;
        std::uint16_t sym = 0;
        std::uint16_t polln = 0, polli = 0;
        bool counted = false;
    };
    std::vector<std::uint64_t> top_cache = std::vector<std::uint64_t>(1u << 17, 0);
    std::uint64_t cur_body = 0;
    std::uint64_t cur_parse = 0, cur_book = 0;
    Order pend[kMaxPerFrame];
    std::size_t pend_n = 0;
    Order flight[kOrderSlots][kMaxPerFrame];
    std::uint16_t flight_n[kOrderSlots] = {};
    bool coalesce = true;
    mintcp::Conn conn;
    std::size_t hdr_len = kHeaderRoom;
    std::uint32_t slot_end[kOrderSlots] = {};
    std::uint64_t slot_at[kOrderSlots] = {};
    std::uint16_t slot_len[kOrderSlots] = {};
    std::uint64_t acked_frames = 0;
    std::uint64_t resends = 0;
    std::uint64_t peer_gone = 0;
    std::uint64_t at_slot[kOrderSlots] = {};
    bool counted[kOrderSlots] = {};
    std::uint16_t before[kOrderSlots] = {};
    sample::Log settle_at, settle_ns;
    hist::Hist latency;
    hist::Hist warmup;
    std::uint64_t window_messages = 0, paced_orders = 0;
    win::Phase was = win::Phase::kGap;
    std::uint64_t drain_mark = 0;
    bool caught_up = false, window_ok = false;
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
    bool every_unit = false;
    std::uint64_t skip = 0;
    std::uint64_t keep = 0;
    sample::Log raw;
    struct WindowStat {
        std::uint32_t samples = 0, over_ms = 0;
        std::uint64_t worst = 0;
    };
    std::vector<WindowStat> per_window = std::vector<WindowStat>(32768);
    std::size_t at_window = 0;

    std::vector<std::uint16_t> raw_window;
    std::vector<std::uint64_t> raw_rx;
    std::vector<std::uint16_t> raw_polln, raw_polli;
    std::uint16_t poll_n = 0, poll_i = 0;
    std::vector<std::uint64_t> stat_when;
    std::vector<std::uint16_t> stat_pkts;
    std::vector<std::uint16_t> stat_msgs;
    std::vector<std::uint32_t> stat_span;
    std::vector<std::uint32_t> stat_gap;
    std::vector<std::uint32_t> stat_proc;
    std::vector<std::uint32_t> stat_send;
    std::vector<std::uint64_t> stat_raw_poll;
    std::vector<std::uint64_t> stat_raw_body;
    std::vector<std::uint64_t> stat_raw_done;
    std::vector<std::uint64_t> stat_raw_nic;
    std::vector<std::uint32_t> stat_blind;
    std::size_t stat_used = 0;
    std::uint32_t poll_msgs = 0;
    std::uint64_t poll_when = 0;
    std::uint64_t last_poll_seen = 0;
    std::uint64_t last_empty = 0;
    std::uint64_t empty_worst[3] = {};
    std::uint64_t empty_over[3] = {};
    std::uint16_t at_polln[kOrderSlots] = {};
    std::uint16_t at_polli[kOrderSlots] = {};
    std::uint16_t at_sym[kOrderSlots] = {};
    std::vector<std::uint16_t> raw_sym;
    std::uint64_t at_poll[kOrderSlots] = {};
    std::uint64_t at_in[kOrderSlots] = {};
    std::uint64_t at_out[kOrderSlots] = {};
    std::vector<std::uint64_t> raw_poll, raw_in, raw_out;
    std::uint64_t at_s1[kOrderSlots] = {};
    std::uint64_t at_s2[kOrderSlots] = {};
    std::uint64_t at_s3[kOrderSlots] = {};
    std::vector<std::uint64_t> raw_s1, raw_s2, raw_s3;
    std::vector<std::uint64_t> raw_body;
    std::vector<std::uint64_t> raw_parse, raw_book;
    std::uint64_t poll_at = 0;
    std::uint16_t dirty[128] = {};
    std::uint64_t dirty_rx[128] = {};
    std::uint16_t dirty_before[128] = {};
    std::uint8_t dirty_paced[128] = {};
    std::uint8_t dirty_measured[128] = {};
    std::uint32_t dirty_n = 0;
    const std::uint8_t* hit_body[kMaxHits] = {};
    std::uint16_t hit_len[kMaxHits] = {};
    std::uint64_t hit_rx[kMaxHits] = {};
    std::uint16_t hit_before[kMaxHits] = {};
    std::uint8_t hit_paced[kMaxHits] = {};
    std::uint8_t hit_measured[kMaxHits] = {};
    std::uint32_t hit_n = 0;

    std::uint32_t bucket_add[kMaxHits] = {};
    std::uint32_t bucket_repl[kMaxHits] = {};
    std::uint32_t bucket_cut[kMaxHits] = {};
    std::uint32_t bucket_del[kMaxHits] = {};
    std::uint32_t add_n = 0, repl_n = 0, cut_n = 0, del_n = 0;

    std::uint32_t repl_new[kMaxHits] = {};
    std::uint32_t repl_old[kMaxHits] = {};

    std::uint32_t look[kMaxHits] = {};
    std::uint32_t keep_slot[kMaxHits] = {};
    std::uint32_t kwant[kMaxHits] = {};
    std::uint32_t zero[kMaxHits] = {};

    bool group = true;
    bool split_ab = false;
    bool collect = true;
    std::uint64_t polls_done = 0;
    std::uint64_t ab_polls[2] = {}, ab_ticks[2] = {}, ab_msgs[2] = {};
    std::vector<std::uint64_t> prev_top = std::vector<std::uint64_t>(1u << 16, 0);
    std::uint64_t depth[65] = {};
    std::vector<std::uint8_t> symbols;
    std::vector<std::uint8_t> traded;
    std::uint64_t bound = 0, unbound = 0;
};

bool read_reference(const char* path, std::unordered_map<std::string, std::uint32_t>* out,
                    std::vector<std::uint32_t>* prices) {
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return false;
    char line[128];
    if (std::fgets(line, sizeof(line), f) == nullptr) {
        std::fclose(f);
        return false;
    }
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* comma = std::strchr(line, ',');
        if (comma == nullptr) continue;
        *comma = '\0';
        const auto price = static_cast<std::uint32_t>(std::strtoul(comma + 1, nullptr, 10));
        (*out)[line] = price;
        prices->push_back(price);
    }
    std::fclose(f);
    return true;
}

void pin(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        std::fprintf(stderr,
                     "cannot run on cpu %d: %s\n"
                     "reserved cores need the process inside the group that owns "
                     "them: scripts/isolate_cores.sh run -- ...\n",
                     cpu, std::strerror(rc));
        std::abort();
    }
}

bool parse_ip(const char* s, std::uint32_t* out) {
    unsigned a, b, c, d;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    *out = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
    return true;
}

bool open_order_path(Shard* self, const char* intf) {
    if (!self->tx.open(intf, 0, 511,
                       static_cast<enum ef_vi_flags>(EF_VI_TX_TIMESTAMPS |
                                                     EF_VI_TX_PUSH_ALWAYS))) {
        return false;
    }
    if (!self->txbuf.alloc(self->tx, kOrderSlots, cfg::kOrderSlotBytes)) return false;
    return true;
}

void seed_slots(Shard* self) {
    for (std::size_t i = 0; i < kOrderSlots; ++i) {
        std::uint8_t* base = self->txbuf.at(i) + self->hdr_len;
        for (std::size_t k = 0; k < kMaxPerFrame; ++k) {
            ouch::prefill(base + k * ouch::kOrderPacketLen);
        }
    }
}

void reap(Shard* self);

void flush_orders(Shard* self) {
    if (self->pend_n == 0) return;
    const std::size_t n = self->pend_n;
    const std::size_t bytes = n * ouch::kOrderPacketLen;
    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    std::uint8_t* msg = slot + self->hdr_len;
    const std::uint64_t f1 = tsc::now();
    std::size_t frame = 0;
    std::uint64_t f2 = 0, f3 = 0, f4 = 0;
    const std::size_t id = self->next_slot;
    {
        frame = self->conn.send(slot, msg, bytes, mintcp::kAck | mintcp::kPsh);
        f2 = tsc::now();
        if (ef_vi_transmit(self->tx.get(), self->txbuf.dma(id),
                           static_cast<int>(frame),
                           static_cast<ef_request_id>(id)) < 0) {
            self->refused += n;
            self->pend_n = 0;
            return;
        }
        self->sent += n;
        f3 = tsc::now();
        f4 = f3;
        self->slot_end[id] = self->conn.snd_nxt();
        self->slot_at[id] = f3;
        self->slot_len[id] = static_cast<std::uint16_t>(frame);
    }
    for (std::size_t k = 0; k < n; ++k) {
        Shard::Order& o = self->pend[k];
        o.s2 = f2; o.s3 = f3; o.out = f4;
        if (o.s1 == 0) o.s1 = f1;
        self->flight[id][k] = o;
    }
    self->flight_n[id] = static_cast<std::uint16_t>(n);
    self->at_slot[id] = self->pend[0].rx;
    self->pend_n = 0;
    ++self->frames;
    self->next_slot = (self->next_slot + 1) % kOrderSlots;
    self->since_send = 0;
}

void send_order(Shard* self, std::uint16_t sym, char side, std::uint64_t rx_ts,
                bool keep, std::uint16_t before, std::uint32_t fixed = 0) {
    const std::uint64_t enter = tsc::now();
    {
        while (self->acked_frames < self->frames &&
               mintcp::before_eq(self->slot_end[self->acked_frames % kOrderSlots],
                                 self->conn.snd_una())) {
            ++self->acked_frames;
        }
        if (self->conn.in_flight() +
                (self->pend_n + 1) * ouch::kOrderPacketLen >
            self->conn.peer_wnd()) {
            const std::uint32_t w = self->conn.peer_wnd();
            if (w < self->wnd_min) self->wnd_min = w;
            if (w > self->wnd_max) self->wnd_max = w;
            ++self->wnd_block;
            ++self->no_slot;
            return;
        }
        if (self->frames - self->acked_frames >= kOrderSlots - 4) {
            if (self->no_slot == 0) {
                std::fprintf(stderr,
                             "own tcp out of slots: frames %" PRIu64 " acked %" PRIu64
                             " reaped %" PRIu64 " una %u nxt %u wnd %u\n",
                             self->frames, self->acked_frames, self->reaped,
                             self->conn.snd_una(), self->conn.snd_nxt(),
                             self->conn.peer_wnd());
            }
            ++self->no_slot;
            return;
        }
    }
    std::uint32_t price = 0, shares = 0;
    const std::uint8_t take_from =
        side == ouch::kBuy ? book::PriceLevels::kSell : book::PriceLevels::kBuy;
    if (fixed != 0) {
        price = fixed;
    } else if (!self->book.best(sym, take_from, &price, &shares)) {
        return;
    }

    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    std::uint8_t* base = slot + self->hdr_len;
    std::uint8_t* msg = base + self->pend_n * ouch::kOrderPacketLen;
    ouch::fill(msg, self->user_ref, side, 1, &self->symbols[sym * 8], price);
    ouch::set_cl_ord_id(msg, self->user_ref);
    ++self->user_ref;
    Shard::Order& o = self->pend[self->pend_n++];
    o.rx = rx_ts;
    o.poll = self->poll_at;
    o.body = self->cur_body;
    o.parse = self->cur_parse;
    o.book = self->cur_book;
    o.in = enter;
    o.s1 = tsc::now();
    o.before = before;
    o.sym = sym;
    o.polln = self->poll_n;
    o.polli = self->poll_i;
    o.counted = keep;
    if (!self->coalesce || self->pend_n == kMaxPerFrame) flush_orders(self);
}

[[gnu::always_inline]] inline std::uint64_t fenced_now() noexcept {
    __builtin_ia32_lfence();
    const std::uint64_t t = tsc::now();
    __builtin_ia32_lfence();
    return t;
}

bool read_neighbour(std::uint32_t peer_ip, std::uint8_t out[6]) {
    std::FILE* f = std::fopen("/proc/net/arp", "r");
    if (f == nullptr) return false;
    char line[256];
    if (std::fgets(line, sizeof(line), f) == nullptr) { std::fclose(f); return false; }
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned a, b, c, d, m[6];
        char flags[16];
        if (std::sscanf(line, "%u.%u.%u.%u %*s %15s %x:%x:%x:%x:%x:%x", &a, &b, &c,
                        &d, flags, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 11) {
            continue;
        }
        if (eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                      static_cast<std::uint8_t>(c),
                      static_cast<std::uint8_t>(d)) != peer_ip) {
            continue;
        }
        if (std::strtol(flags, nullptr, 16) == 0) continue;
        for (int i = 0; i < 6; ++i) out[i] = static_cast<std::uint8_t>(m[i]);
        found = true;
        break;
    }
    std::fclose(f);
    return found;
}

void take_ack(Shard* self, const std::uint8_t* p, std::size_t len) {
    if (len < mintcp::kHeaderLen) return;
    if ((p[mintcp::kTcpFlagsOff] & (mintcp::kRst | mintcp::kFin)) != 0) {
        ++self->peer_gone;
    }
    if ((p[mintcp::kTcpFlagsOff] & mintcp::kAck) == 0) return;
    (void)self->conn.on_ack(mintcp::get32(p + mintcp::kTcpAckOff),
                            mintcp::get16(p + mintcp::kTcpWinOff));
}

void resend_stale(Shard* self, std::uint64_t rto) {
    while (self->acked_frames < self->frames &&
           mintcp::before_eq(self->slot_end[self->acked_frames % kOrderSlots],
                             self->conn.snd_una())) {
        ++self->acked_frames;
    }
    if (self->acked_frames == self->frames) return;
    const std::size_t id = self->acked_frames % kOrderSlots;
    if (tsc::now() - self->slot_at[id] < rto) return;
    (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(id),
                         static_cast<int>(self->slot_len[id]),
                         static_cast<ef_request_id>(id));
    self->slot_at[id] = tsc::now();
    ++self->resends;
}

bool shake_hands(Shard* self, ef::Vi& rx, ef::Frames& rxbuf, std::size_t prefix,
                 std::size_t rx_slots, std::size_t* next_post,
                 std::uint32_t our_ip, std::uint16_t our_port,
                 std::uint32_t peer_ip, std::uint16_t peer_port,
                 const char* intf) {
    (void)intf;
    std::uint8_t our_mac[6];
    if (ef_vi_get_mac(self->tx.get(), self->tx.dh(), our_mac) < 0) return false;
    std::uint8_t peer_mac[6];
    if (!read_neighbour(peer_ip, peer_mac)) return false;

    eth::Endpoint us{}, them{};
    std::memcpy(us.mac, our_mac, 6);
    us.ip = our_ip;
    us.port = our_port;
    them.ip = peer_ip;
    them.port = peer_port;
    self->conn.open(us, them, peer_mac, static_cast<std::uint32_t>(tsc::now()),
                    65535);
    self->hdr_len = mintcp::kHeaderLen;

    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    std::size_t n = self->conn.send_syn(slot, 0);
    if (ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                       static_cast<int>(n),
                       static_cast<ef_request_id>(self->next_slot)) < 0) {
        return false;
    }
    self->next_slot = (self->next_slot + 1) % kOrderSlots;

    const double tps = tsc::ticks_per_ns();
    const std::uint64_t stop = tsc::now() + static_cast<std::uint64_t>(3e9 * tps);
    while (tsc::now() < stop) {
        ef_event evs[8];
        const int got = ef_eventq_poll(rx.get(), evs, 8);
        for (int i = 0; i < got; ++i) {
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
            const std::size_t id = EF_EVENT_RX_RQ_ID(evs[i]);
            const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - prefix;
            const std::uint8_t* p = rxbuf.at(id) + prefix;
            bool done = false;
            if (len >= mintcp::kHeaderLen &&
                (p[mintcp::kTcpFlagsOff] & mintcp::kSyn) != 0 &&
                (p[mintcp::kTcpFlagsOff] & mintcp::kAck) != 0) {
                self->conn.set_rcv_nxt(mintcp::get32(p + mintcp::kTcpSeqOff) + 1);
                (void)self->conn.on_ack(mintcp::get32(p + mintcp::kTcpAckOff),
                                        mintcp::get16(p + mintcp::kTcpWinOff));
                self->conn.set_peer_shift(mintcp::Conn::shift_in(p, len));
                done = true;
            }
            const std::size_t give = *next_post % rx_slots;
            ++*next_post;
            (void)ef_vi_receive_init(rx.get(), rxbuf.dma(give), give);
            ef_vi_receive_push(rx.get());
            if (done) {
                slot = self->txbuf.at(self->next_slot);
                n = self->conn.send(slot, nullptr, 0, mintcp::kAck);
                (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                                     static_cast<int>(n),
                                     static_cast<ef_request_id>(self->next_slot));
                self->next_slot = (self->next_slot + 1) % kOrderSlots;
                self->conn.on_ack(self->conn.snd_nxt(), self->conn.peer_wnd());
                self->frames += 2;
                self->acked_frames += 2;
                seed_slots(self);
                return true;
            }
        }
    }
    return false;
}

void warm(Shard* self) {
    ef_vi_start_transmit_warm(self->tx.get(), &self->warm_state, nullptr);
    (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                         static_cast<int>(kHeaderRoom + ouch::kOrderPacketLen),
                         EF_REQUEST_ID_MASK);
    ef_vi_stop_transmit_warm(self->tx.get(), &self->warm_state);
    ++self->warmed;
}

void reap(Shard* self) {
    ef_event evs[16];
    const int n = ef_eventq_poll(self->tx.get(), evs, 16);
    for (int i = 0; i < n; ++i) {
        if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_TX_WITH_TIMESTAMP) continue;
        const std::size_t id = EF_EVENT_TX_WITH_TIMESTAMP_RQ_ID(evs[i]) % kOrderSlots;
        const std::uint64_t out =
            EF_EVENT_TX_WITH_TIMESTAMP_SEC(evs[i]) * 1000000000ull +
            EF_EVENT_TX_WITH_TIMESTAMP_NSEC(evs[i]);
        for (std::size_t k = 0; k < self->flight_n[id]; ++k) {
            const Shard::Order& o = self->flight[id][k];
            const std::uint64_t in = o.rx;
            if (in == 0 || out <= in) continue;
            (o.counted ? self->latency : self->warmup).add(out - in);
            if (!o.counted && o.before != 0) {
                self->settle_at.add(o.before);
                self->settle_ns.add(out - in);
            }
            if (o.counted) {
                self->raw.add(out - in);
                if (self->raw_window.size() < self->raw_window.capacity()) {
                    self->raw_window.push_back(static_cast<std::uint16_t>(self->at_window));
                    self->raw_rx.push_back(in);
                    self->raw_polln.push_back(o.polln);
                    self->raw_polli.push_back(o.polli);
                    self->raw_sym.push_back(o.sym);
                    self->raw_poll.push_back(o.poll);
                    self->raw_in.push_back(o.in);
                    self->raw_out.push_back(o.out);
                    self->raw_s1.push_back(o.s1);
                    self->raw_s2.push_back(o.s2);
                    self->raw_body.push_back(o.body);
                    self->raw_parse.push_back(o.parse);
                    self->raw_book.push_back(o.book);
                    self->raw_s3.push_back(o.s3);
                }
                if (self->at_window < self->per_window.size()) {
                    Shard::WindowStat& w = self->per_window[self->at_window];
                    ++w.samples;
                    if (out - in > 1000000) ++w.over_ms;
                    if (out - in > w.worst) w.worst = out - in;
                }
            }
            ++self->stamped;
        }
        self->flight_n[id] = 0;
        self->at_slot[id] = 0;
        ++self->reaped;
    }
}

void settle_dirty(Shard* self, bool trading) {
    for (std::uint32_t i = 0; i < self->dirty_n; ++i) {
        const std::uint16_t sym = self->dirty[i];
        const std::uint64_t bid3 = self->book.top3(sym, book::PriceLevels::kBuy);
        const std::uint64_t ask3 = self->book.top3(sym, book::PriceLevels::kSell);
        const auto what = self->signal.check(bid3, ask3);
        const std::uint64_t was = self->prev_top[sym];
        self->prev_top[sym] = (bid3 << 32) | (ask3 & 0xffffffffull);
        if (what == book::Imbalance::Signal::kNone) continue;
        const std::uint64_t wb = was >> 32, ws = was & 0xffffffffull;
        if (wb == 0 && ws == 0) continue;
        const bool up = bid3 * (wb + ws) > wb * (bid3 + ask3);
        if (what == book::Imbalance::Signal::kBuy) {
            if (!up) continue;
        } else {
            const bool down = bid3 * (wb + ws) < wb * (bid3 + ask3);
            if (!down) continue;
        }
        const char side =
            what == book::Imbalance::Signal::kBuy ? ouch::kBuy : ouch::kSell;
        if (side == ouch::kBuy) ++self->buys; else ++self->sells;
        if (self->dirty_paced[i] == 0) continue;
        ++self->paced_orders;
        if (trading) send_order(self, sym, side, self->dirty_rx[i],
                                self->dirty_measured[i] != 0, self->dirty_before[i]);
    }
    self->dirty_n = 0;
}

void note_applied(Shard* self, std::uint16_t touched, std::uint64_t rx,
                  std::uint8_t paced, std::uint8_t measured, std::uint16_t before,
                  bool trading) {
    ++self->applied;
    if (measured != 0) ++self->applied_window;
    if (trading && ++self->since_send >= 256) {
        warm(self);
        self->since_send = 0;
    }
    std::uint32_t d = 0;
    while (d < self->dirty_n && self->dirty[d] != touched) ++d;
    const bool fresh = d == self->dirty_n;
    if (fresh && self->dirty_n < 128) {
        self->dirty[self->dirty_n++] = touched;
    }
    if (d < 128) {
        if (fresh) {
            self->dirty_rx[d] = rx;
        } else if (rx < self->dirty_rx[d]) {
            self->dirty_rx[d] = rx;
        }
        self->dirty_paced[d] = paced;
        self->dirty_measured[d] = measured;
        self->dirty_before[d] = before;
    }
}

void apply_stream(Shard* self, bool trading) {
    for (std::uint32_t i = 0; i < self->hit_n; ++i) {
        const itch::Message m{self->hit_body[i], self->hit_len[i]};
        std::uint16_t touched = 0;
        if (self->book.apply(m, &touched)) {
            note_applied(self, touched, self->hit_rx[i], self->hit_paced[i],
                         self->hit_measured[i], self->hit_before[i], trading);
        }
    }
}

void apply_grouped(Shard* self, bool trading) {
    for (std::uint32_t k = 0; k < self->add_n; ++k) {
        const std::uint32_t i = self->bucket_add[k];
        const std::uint8_t* b = self->hit_body[i];
        const std::uint16_t sym = itch::read_be<std::uint16_t>(b + itch::kLocateOff);
        const book::OrderTable::Order o{
            itch::read_be<std::uint32_t>(b + itch::kAddSharesOff),
            itch::read_be<std::uint32_t>(b + itch::kAddPriceOff),
            static_cast<std::uint8_t>(b[itch::kAddSideOff] == 'B' ? 0 : 1), sym};
        self->book.insert_at(itch::read_be<std::uint64_t>(b + itch::kAddRefOff), o);
        self->book.level_move(sym, o.side, o.price, static_cast<std::int64_t>(o.shares));
        note_applied(self, sym, self->hit_rx[i], self->hit_paced[i],
                     self->hit_measured[i], self->hit_before[i], trading);
    }
    for (std::uint32_t k = 0; k < self->repl_n; ++k) {
        const std::uint8_t* b = self->hit_body[self->bucket_repl[k]];
        const book::OrderTable::Order o{
            itch::read_be<std::uint32_t>(b + itch::kReplaceSharesOff),
            itch::read_be<std::uint32_t>(b + itch::kReplacePriceOff), 0, 0};
        self->repl_new[k] =
            self->book.insert_at(itch::read_be<std::uint64_t>(b + itch::kReplaceNewRefOff), o);
    }
    std::uint32_t missing = 0;
    for (std::uint32_t k = 0; k < self->repl_n; ++k) {
        const std::uint8_t* b = self->hit_body[self->bucket_repl[k]];
        const std::uint32_t slot =
            self->book.find_slot(itch::read_be<std::uint64_t>(b + itch::kReplaceOldRefOff));
        self->repl_old[k] = slot;
        missing += slot == book::OrderTable::kNoSlot ? 1u : 0u;
    }
    if (missing == 0) {
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.set_side_sym_at(self->repl_new[k], o.side, o.sym);
        }
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->repl_old[k]);
            const book::OrderTable::Order fresh = self->book.at(self->repl_new[k]);
            self->book.level_move(fresh.sym, fresh.side, fresh.price,
                                  static_cast<std::int64_t>(fresh.shares));
            note_applied(self, fresh.sym, self->hit_rx[self->bucket_repl[k]],
                         self->hit_paced[self->bucket_repl[k]],
                         self->hit_measured[self->bucket_repl[k]],
                         self->hit_before[self->bucket_repl[k]], trading);
        }
    } else {
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            if (self->repl_old[k] == book::OrderTable::kNoSlot) continue;
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.set_side_sym_at(self->repl_new[k], o.side, o.sym);
        }
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            if (self->repl_old[k] == book::OrderTable::kNoSlot) {
                self->book.erase_at(self->repl_new[k]);
                continue;
            }
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->repl_old[k]);
            const book::OrderTable::Order fresh = self->book.at(self->repl_new[k]);
            self->book.level_move(fresh.sym, fresh.side, fresh.price,
                                  static_cast<std::int64_t>(fresh.shares));
            note_applied(self, fresh.sym, self->hit_rx[self->bucket_repl[k]],
                         self->hit_paced[self->bucket_repl[k]],
                         self->hit_measured[self->bucket_repl[k]],
                         self->hit_before[self->bucket_repl[k]], trading);
        }
    }
    {
        for (std::uint32_t k = 0; k < self->del_n; ++k) {
            self->look[k] = self->book.find_slot(itch::read_be<std::uint64_t>(
                self->hit_body[self->bucket_del[k]] + itch::kDeleteRefOff));
        }
        std::uint32_t n = 0;
        for (std::uint32_t k = 0; k < self->del_n; ++k) {
            self->keep_slot[n] = self->look[k];
            self->kwant[n] = self->bucket_del[k];
            n += self->look[k] != book::OrderTable::kNoSlot ? 1u : 0u;
        }
        for (std::uint32_t j = 0; j < n; ++j) {
            const book::OrderTable::Order o = self->book.at(self->keep_slot[j]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->keep_slot[j]);
            const std::uint32_t i = self->kwant[j];
            note_applied(self, o.sym, self->hit_rx[i], self->hit_paced[i],
                         self->hit_measured[i], self->hit_before[i], trading);
        }
    }
    {
        for (std::uint32_t k = 0; k < self->cut_n; ++k) {
            const std::uint8_t* b = self->hit_body[self->bucket_cut[k]];
            self->look[k] = self->book.find_slot(
                itch::read_be<std::uint64_t>(b + itch::kExecRefOff));
            self->kwant[k] = itch::read_be<std::uint32_t>(b + itch::kExecSharesOff);
        }
        std::uint32_t n = 0;
        for (std::uint32_t k = 0; k < self->cut_n; ++k) {
            self->keep_slot[n] = self->look[k];
            self->kwant[n] = self->kwant[k];
            self->zero[n] = self->bucket_cut[k];
            n += self->look[k] != book::OrderTable::kNoSlot ? 1u : 0u;
        }
        std::uint32_t z = 0;
        for (std::uint32_t j = 0; j < n; ++j) {
            const book::OrderTable::Order o = self->book.at(self->keep_slot[j]);
            const std::uint32_t off = self->kwant[j] < o.shares ? self->kwant[j] : o.shares;
            self->book.set_shares_at(self->keep_slot[j], o.shares - off);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(off));
            const std::uint32_t i = self->zero[j];
            note_applied(self, o.sym, self->hit_rx[i], self->hit_paced[i],
                         self->hit_measured[i], self->hit_before[i], trading);
            self->look[z] = self->keep_slot[j];
            z += o.shares == off ? 1u : 0u;
        }
        for (std::uint32_t q = 0; q < z; ++q) self->book.erase_at(self->look[q]);
    }
    self->add_n = self->repl_n = self->cut_n = self->del_n = 0;
}

void apply_hits(Shard* self, bool trading) {
    if (self->group) apply_grouped(self, trading);
    else apply_stream(self, trading);
    self->hit_n = 0;
}

void take_packet(Shard* self, const std::uint8_t* buf, std::uint32_t len,
                 std::uint64_t hw_ts,
                 const std::unordered_map<std::string, std::uint32_t>* reference,
                 const std::unordered_set<std::string>* wanted,
                 bool trading, const std::atomic<std::uint64_t>* drained) {
    const std::uint8_t* traded = self->traded.empty() ? nullptr : self->traded.data();
    const auto mine = [=](std::uint16_t sym) {
        if (traded != nullptr && traded[sym] == 0) return false;
        return true;
    };
    {
        if (len < eth::kHeaderBytes + mold::kHeaderLen) return;
        const std::uint8_t* p = buf + eth::kHeaderBytes;
        const std::uint64_t seq = mold::sequence(p);
        const std::uint16_t count = mold::count(p);

        if (!self->started) {
            self->started = true;
            self->expect_seq = seq;
        } else if (seq < self->expect_seq) {
            ++self->duplicates;
            return;
        } else if (seq > self->expect_seq) {
            ++self->gaps;
        }
        self->expect_seq = seq + count;

        const std::size_t payload = len - eth::kHeaderBytes - mold::kHeaderLen;
        (void)itch::for_each_message(
            p + mold::kHeaderLen, payload, [&](const itch::Message& m) {
                ++self->messages;
                if (self->poll_msgs == 0) self->poll_when = m.timestamp();
                ++self->poll_msgs;
                win::note_session(m, &self->phase);
                const win::Phase where = self->phase.advance(m.timestamp());
                if (self->every_unit) {
                    const std::uint64_t u = self->phase.index();
                    if (u != self->at_window) {
                        ++self->windows;
                        self->at_window = u;
                        self->window_ok = true;
                    }
                }
                if (where != self->was) {
                    if (where == win::Phase::kSettle) {
                        self->caught_up = false;
                        self->drain_mark = drained->load(std::memory_order_relaxed);
                    } else if (where == win::Phase::kWindow) {
                        ++self->windows;
                        self->at_window = self->windows;
                        self->window_ok =
                            self->caught_up &&
                            drained->load(std::memory_order_relaxed) != self->drain_mark;
                        if (!self->window_ok) ++self->windows_dropped;
                        if (self->windows <= self->skip) self->window_ok = false;
                        if (self->keep != 0 && self->windows > self->skip + self->keep) {
                            self->window_ok = false;
                        }
                    }
                    self->was = where;
                }
                const bool paced = win::Tracker::one_to_one(where);
                const bool measured = where == win::Phase::kWindow && self->window_ok;
                if (where == win::Phase::kWindow) {
                    ++self->window_messages;
                    if (!self->window_ok) ++self->dropped_samples;
                }
                const std::uint16_t sym = m.stock_locate();
                if (m.type() == 'R') {
                    const char* s2 =
                        reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                    std::size_t n = itch::kStockSymbolLen;
                    while (n > 0 && s2[n - 1] == ' ') --n;
                    if (!self->traded.empty()) {
                        self->traded[sym] =
                            wanted->count(std::string(s2, n)) != 0 ? 1 : 0;
                    }
                    if (!mine(sym)) return true;
                    std::memcpy(&self->symbols[sym * 8], s2, itch::kStockSymbolLen);
                    const auto it = reference->find(std::string(s2, n));
                    if (it != reference->end() && self->book.bind(sym, it->second)) {
                        ++self->bound;
                    } else {
                        ++self->unbound;
                    }
                    return true;
                }
                if (!mine(sym)) return true;
                const std::uint16_t before =
                    where == win::Phase::kSettle
                        ? static_cast<std::uint16_t>(
                              (self->phase.open() - m.timestamp()) / 10000000 + 1)
                        : 0;
                if (!self->collect) {
                    std::uint16_t touched = 0;
                    if (self->book.apply(m, &touched)) {
                        note_applied(self, touched, hw_ts, paced ? 1 : 0,
                                     measured ? 1 : 0, before, trading);
                    }
                    return true;
                }
                if (self->hit_n == kMaxHits) apply_hits(self, trading);
                if (self->group) {
                    switch (m.type()) {
                        case 'A': case 'F': self->bucket_add[self->add_n++] = self->hit_n; break;
                        case 'U': self->bucket_repl[self->repl_n++] = self->hit_n; break;
                        case 'E': case 'C': case 'X': self->bucket_cut[self->cut_n++] = self->hit_n; break;
                        case 'D': self->bucket_del[self->del_n++] = self->hit_n; break;
                        default: break;
                    }
                }
                self->hit_body[self->hit_n] = m.body;
                self->hit_len[self->hit_n] = m.len;
                self->hit_rx[self->hit_n] = hw_ts;
                self->hit_paced[self->hit_n] = paced ? 1 : 0;
                self->hit_measured[self->hit_n] = measured ? 1 : 0;
                self->hit_before[self->hit_n] = before;
                ++self->hit_n;
                return true;
            });
    }
}

}

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const bool has = i + 1 < argc;
        const char* a = argv[i];
        if (std::strcmp(a, "--intf") == 0 && has) opt.intf = argv[++i];
        else if (std::strcmp(a, "--reference") == 0 && has) opt.reference = argv[++i];
        else if (std::strcmp(a, "--symbols") == 0 && has) opt.symbols = argv[++i];
        else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &opt.dst_ip)) return 2;
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            opt.dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--dst-port-b") == 0 && has) {
            opt.dst_port_b = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--cpu-base") == 0 && has) {
            opt.cpu_base = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--threshold") == 0 && has) {
            opt.threshold = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--one-at-a-time") == 0) {
            opt.group = false;
        } else if (std::strcmp(a, "--split-ab") == 0) {
            opt.split_ab = true;
        } else if (std::strcmp(a, "--stat") == 0 && has) {
            opt.stat = argv[++i];
        } else if (std::strcmp(a, "--segments") == 0) {
            opt.segments = true;
        } else if (std::strcmp(a, "--local-ip") == 0 && has) {
            opt.order_local = argv[++i];
        } else if (std::strcmp(a, "--idle-ms") == 0 && has) {
            opt.idle_ms = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--order-ip") == 0 && has) {
            opt.order_ip = argv[++i];
        } else if (std::strcmp(a, "--order-port") == 0 && has) {
            opt.order_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--out") == 0 && has) {
            opt.out = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: trader [--intf I] [--reference CSV] [--symbols FILE]\n"
                         "              [--dst-ip A.B.C.D] [--dst-port N] [--dst-port-b N]\n"
                         "              [--cpu-base N] [--threshold N] [--idle-ms N]\n"
                         "              [--order-ip A.B.C.D] [--order-port N]\n"
                         "              [--local-ip A.B.C.D] [--out DIR] [--stat DIR]\n"
                         "              [--split-ab] [--one-at-a-time]\n"
                         "  ITCH_SKIP_WINDOWS / ITCH_MAX_WINDOWS pick which windows count\n"
                         "  sending orders needs onload in front of it\n");
            return 2;
        }
    }

    std::unordered_map<std::string, std::uint32_t> reference;
    std::vector<std::uint32_t> prices;
    if (!read_reference(opt.reference, &reference, &prices)) {
        std::fprintf(stderr, "cannot read %s\n", opt.reference);
        return 1;
    }

    std::unordered_set<std::string> wanted;
    if (opt.symbols != nullptr) {
        std::FILE* f = std::fopen(opt.symbols, "r");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot read %s\n", opt.symbols);
            return 1;
        }
        char line[64];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
                s.pop_back();
            }
            if (!s.empty()) wanted.insert(s);
        }
        std::fclose(f);
        if (wanted.empty()) {
            std::fprintf(stderr, "%s named no securities\n", opt.symbols);
            return 1;
        }
        std::vector<std::uint32_t> keep;
        for (const auto& n : wanted) {
            const auto it = reference.find(n);
            if (it != reference.end()) keep.push_back(it->second);
        }
        std::printf("universe       %zu names, %zu of them with a reference price\n",
                    wanted.size(), keep.size());
        prices.swap(keep);
    }

    pin(opt.cpu_base);
    huge::choose(huge::node_of_cpu(opt.cpu_base));
    std::printf("huge pages     taken from node %d, the one cpu %d is on\n",
                huge::node_of_cpu(opt.cpu_base), opt.cpu_base);

    ef::Vi vi;
    if (!vi.open(opt.intf, static_cast<int>(cfg::kRxDescriptors), 0, EF_VI_RX_TIMESTAMPS)) {
        return 1;
    }
    ef_vi_receive_set_buffer_len(vi.get(), cfg::kRxSlotBytes);
    ef::Frames frames;
    if (!frames.alloc(vi, cfg::kRxRingSlots, cfg::kRxSlotBytes)) return 1;

    for (std::uint16_t port : {opt.dst_port, opt.dst_port_b}) {
        if (port == 0) continue;
        ef_filter_spec fs;
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        int rc = ef_filter_spec_set_ip4_local(&fs, IPPROTO_UDP, htonl(opt.dst_ip),
                                              static_cast<int>(htons(port)));
        ef_filter_cookie cookie;
        if (rc < 0 || (rc = ef_vi_filter_add(vi.get(), vi.dh(), &fs, &cookie)) < 0) {
            std::fprintf(stderr, "cannot listen on port %u: %d\n", port, rc);
            return 1;
        }
    }

    std::printf("signal         once per poll, %s\n",
                "and only when the imbalance moved further that way");
    std::printf("order path     %s\n",
                "our own TCP");
    std::printf("book path      %s\n",
                opt.group ? "grouped in seven passes" : "one message at a time");
    std::uint32_t peer_ip = 0, local_ip = 0;
    ef::Vi ack_vi;
    ef::Frames ack_frames;
    constexpr std::size_t kAckRingSlots = 512;
    constexpr std::size_t kAckSlotBytes = 2048;
    std::size_t ack_prefix = 0;
    std::size_t ack_post = kAckRingSlots - 1;
    {
        if (opt.order_ip == nullptr || !parse_ip(opt.order_ip, &peer_ip) ||
            !parse_ip(opt.order_local, &local_ip)) {
            std::fprintf(stderr, "own tcp needs both addresses\n");
            return 1;
        }
        if (!ack_vi.open(opt.intf, static_cast<int>(kAckRingSlots), 0,
                         EF_VI_FLAGS_DEFAULT)) {
            return 1;
        }
        ef_vi_receive_set_buffer_len(ack_vi.get(), kAckSlotBytes);
        if (!ack_frames.alloc(ack_vi, kAckRingSlots, kAckSlotBytes)) return 1;
        ack_prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(ack_vi.get()));
        ef_filter_spec fs;
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        const std::uint16_t lp = static_cast<std::uint16_t>(51000 + 0);
        const std::uint16_t rp = static_cast<std::uint16_t>(opt.order_port + 0);
        ef_filter_cookie ck;
        if (ef_filter_spec_set_ip4_full(&fs, IPPROTO_TCP, htonl(local_ip),
                                        static_cast<int>(htons(lp)),
                                        htonl(peer_ip),
                                        static_cast<int>(htons(rp))) < 0 ||
            ef_vi_filter_add(ack_vi.get(), ack_vi.dh(), &fs, &ck) < 0) {
            std::fprintf(stderr, "cannot listen for the order connection\n");
            return 1;
        }
        for (std::size_t k = 0; k + 1 < kAckRingSlots; ++k) {
            if (ef_vi_receive_init(ack_vi.get(), ack_frames.dma(k), k) < 0) return 1;
        }
        ef_vi_receive_push(ack_vi.get());
    }

    const std::size_t prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(vi.get()));
    std::size_t next_post = 0;
    for (; next_post < cfg::kRxDescriptors - 1; ++next_post) {
        if (ef_vi_receive_init(vi.get(), frames.dma(next_post), next_post) < 0) return 1;
    }
    ef_vi_receive_push(vi.get());

    const char* order_cap_env = std::getenv("ITCH_ORDER_CAP");
    const std::size_t orders_each =
        order_cap_env != nullptr ? std::strtoull(order_cap_env, nullptr, 10)
                                 : (12u << 20);
    const std::size_t whole = book::PriceLevels::budget_for(prices);
    const std::size_t words_each =
        opt.symbols != nullptr ? whole + (1u << 20)
                               : whole * 3 / 2 + (1u << 20);
    std::printf("%.2f GB of prices and %zu orders\n",
                words_each * 8.0 / 1e9, orders_each);

    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> drained{0};
    std::vector<std::unique_ptr<Shard>> shards;
    if (opt.lock_memory) {
        mallopt(M_TRIM_THRESHOLD, -1);
        mallopt(M_MMAP_MAX, 0);
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            std::perror("mlockall");
            return 1;
        }
        std::printf("memory held in place\n");
    }
    const win::Params wp = win::params_from_env();
    const char* cap_env = std::getenv("ITCH_SAMPLE_CAP");
    const std::size_t sample_cap =
        cap_env != nullptr ? std::strtoull(cap_env, nullptr, 10) : (4u << 20);
    const char* skip_env = std::getenv("ITCH_SKIP_WINDOWS");
    const std::uint64_t skip = skip_env != nullptr ? std::strtoull(skip_env, nullptr, 10) : 0;
    const char* keep_env = std::getenv("ITCH_MAX_WINDOWS");
    const std::uint64_t keep = keep_env != nullptr ? std::strtoull(keep_env, nullptr, 10) : 0;
    shards.push_back(
        std::make_unique<Shard>(orders_each, words_each, opt.threshold, wp));
    shards.back()->split_ab = opt.split_ab;
    shards.back()->group = opt.group;
    shards.back()->skip = skip;
    shards.back()->keep = keep;
    shards.back()->raw.reserve(sample_cap);
    shards.back()->raw_window.assign(sample_cap, 0);
    shards.back()->raw_window.clear();
    shards.back()->raw_rx.assign(sample_cap, 0);
    shards.back()->raw_rx.clear();
    shards.back()->raw_polln.assign(sample_cap, 0);
    shards.back()->raw_polln.clear();
    shards.back()->raw_polli.assign(sample_cap, 0);
    shards.back()->raw_polli.clear();
    shards.back()->raw_sym.assign(sample_cap, 0);
    shards.back()->raw_sym.clear();
    shards.back()->raw_poll.assign(sample_cap, 0);
    shards.back()->raw_poll.clear();
    shards.back()->raw_in.assign(sample_cap, 0);
    shards.back()->raw_in.clear();
    shards.back()->raw_out.assign(sample_cap, 0);
    shards.back()->raw_out.clear();
    shards.back()->raw_s1.assign(sample_cap, 0);
    shards.back()->raw_s1.clear();
    shards.back()->raw_s2.assign(sample_cap, 0);
    shards.back()->raw_s2.clear();
    shards.back()->raw_body.assign(sample_cap, 0);
    shards.back()->raw_body.clear();
    shards.back()->raw_parse.assign(sample_cap, 0);
    shards.back()->raw_parse.clear();
    shards.back()->raw_book.assign(sample_cap, 0);
    shards.back()->raw_book.clear();
    shards.back()->raw_s3.assign(sample_cap, 0);
    shards.back()->raw_s3.clear();
    shards.back()->settle_at.reserve(sample_cap);
    shards.back()->settle_ns.reserve(sample_cap);
    if (!wanted.empty()) shards.back()->traded.assign(65536, 0);
    shards.back()->coalesce = opt.coalesce;
    shards.back()->every_unit = wp.mask == 0;

    if (opt.stat != nullptr && !shards.empty()) {
        constexpr std::size_t kStatCap = 80u << 20;
        shards[0]->stat_when.assign(kStatCap, 0);
        shards[0]->stat_pkts.assign(kStatCap, 0);
        shards[0]->stat_msgs.assign(kStatCap, 0);
        shards[0]->stat_span.assign(kStatCap, 0);
        shards[0]->stat_gap.assign(kStatCap, 0);
        shards[0]->stat_proc.assign(kStatCap, 0);
        shards[0]->stat_send.assign(kStatCap, 0);
        shards[0]->stat_raw_poll.assign(kStatCap, 0);
        shards[0]->stat_raw_body.assign(kStatCap, 0);
        shards[0]->stat_raw_done.assign(kStatCap, 0);
        shards[0]->stat_raw_nic.assign(kStatCap, 0);
        shards[0]->stat_blind.assign(kStatCap, 0);
        std::printf("stat           room for %zu polls, %.1f GB, pre-touched\n",
                    kStatCap, kStatCap * 12.0 / (1u << 30));
    }
    if (skip != 0 || keep != 0) {
        std::printf("counting windows %llu..%s\n",
                    static_cast<unsigned long long>(skip + 1),
                    keep == 0 ? "end"
                              : std::to_string(skip + keep).c_str());
    }
    const bool trading = opt.order_ip != nullptr;
    if (trading) {
        if (!open_order_path(shards[0].get(), opt.intf)) {
            std::fprintf(stderr, "could not open the order path\n");
            return 1;
        }
        {
            if (!shake_hands(shards[0].get(), ack_vi, ack_frames, ack_prefix,
                             kAckRingSlots, &ack_post, local_ip,
                             static_cast<std::uint16_t>(51000 + 0), peer_ip,
                             static_cast<std::uint16_t>(opt.order_port + 0),
                             opt.intf)) {
                std::fprintf(stderr, "could not open our own connection\n");
                return 1;
            }
            std::printf("own connection opened, they start at %u\n",
                        shards[0]->conn.rcv_nxt());
            std::printf("window scale   they shift by %u, so at most %u bytes"
                        " may be unacknowledged\n",
                        shards[0]->conn.peer_shift(),
                        65535u << shards[0]->conn.peer_shift());
        }
    }
    const bool stat_on = opt.stat != nullptr;
    const bool timing = stat_on || opt.segments;
    std::printf("segments       %s\n",
                timing ? "on, six segments timed per poll"
                       : "off, no per-poll clock reads");
    const std::uint64_t rto_ticks =
        static_cast<std::uint64_t>(1e6 * tsc::ticks_per_ns());

    std::thread ack_thread;
    if (trading) {
        const int ack_cpu = opt.cpu_base < 0 ? -1 : opt.cpu_base + 1;
        ack_thread = std::thread([&, ack_cpu] {
            if (ack_cpu >= 0) pin(ack_cpu);
            std::size_t give = ack_post & (kAckRingSlots - 1);
            while (!done.load(std::memory_order_acquire)) {
                ef_event evs[16];
                const int n = ef_eventq_poll(ack_vi.get(), evs, 16);
                for (int i = 0; i < n; ++i) {
                    if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
                    const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
                    const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - ack_prefix;
                    take_ack(shards[0].get(), ack_frames.at(slot) + ack_prefix, len);
                    if (ef_vi_receive_init(ack_vi.get(), ack_frames.dma(give), give) >= 0) {
                        give = (give + 1) & (kAckRingSlots - 1);
                    }
                }
                if (n > 0) ef_vi_receive_push(ack_vi.get());
            }
        });
        std::printf("ack path       its own queue on %s, polled by a thread on cpu %d\n",
                    opt.intf, ack_cpu);
    }

    nic::Drops before;
    const bool have_counters = nic::read_drops(opt.intf, &before);

    const double tps = tsc::ticks_per_ns();
    const std::uint64_t idle_ticks = static_cast<std::uint64_t>(opt.idle_ms * 1e6 * tps);
    std::uint64_t last_seen = tsc::now(), start = 0;
    std::uint64_t packets = 0, discards = 0;
    std::uint64_t poll_seq = 0;
    ef_event evs[kMaxPollEvents];

    for (;;) {
        const int n = ef_eventq_poll(vi.get(), evs, kMaxPollEvents);
        if (n == 0) {
            if (stat_on) shards[0]->last_empty = tsc::now();
            const std::uint64_t e0 = stat_on ? tsc::now() : 0;
            drained.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t e1 = stat_on ? tsc::now() : 0;
            shards[0]->caught_up = true;
            if (trading) {
                reap(shards[0].get());
                const std::uint64_t e2 = stat_on ? tsc::now() : 0;
                resend_stale(shards[0].get(), rto_ticks);
                if (stat_on) {
                    const std::uint64_t e3 = tsc::now();
                    Shard* sh = shards[0].get();
                    const std::uint64_t d[3] = {
                        static_cast<std::uint64_t>((e1 - e0) / tps),
                        static_cast<std::uint64_t>((e2 - e1) / tps),
                        static_cast<std::uint64_t>((e3 - e2) / tps)};
                    for (int q = 0; q < 3; ++q) {
                        if (d[q] > sh->empty_worst[q]) sh->empty_worst[q] = d[q];
                        if (d[q] >= 10000) ++sh->empty_over[q];
                    }
                }
            }
            if (packets != 0 && tsc::now() - last_seen > idle_ticks) break;
            if (packets == 0 && tsc::now() - last_seen > idle_ticks * 15) {
                std::fprintf(stderr, "nothing arrived\n");
                done.store(true, std::memory_order_release);
                if (ack_thread.joinable()) ack_thread.join();
                return 1;
            }
            continue;
        }
        last_seen = tsc::now();
        ++shards[0]->depth[n < 65 ? n : 64];
        const std::uint64_t g1 = timing ? fenced_now() : 0;
        for (int i = 0; i < n; ++i) {
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
            const std::uint8_t* b = frames.at(EF_EVENT_RX_RQ_ID(evs[i]));
            __builtin_prefetch(b, 0, 3);
            __builtin_prefetch(b + 64, 0, 3);
        }
        const std::uint64_t g2 = timing ? fenced_now() : 0;
        shards[0]->cur_body = g2;
        shards[0]->poll_msgs = 0; shards[0]->poll_when = 0;
        ++poll_seq;
        const bool in_window = shards[0]->was == win::Phase::kWindow;
        const std::uint64_t poll_tsc_always = in_window ? g1 : 0;
        const std::uint64_t stat_poll_tsc =
            (stat_on && poll_tsc_always == 0) ? tsc::now() : poll_tsc_always;
        std::uint64_t first_body = 0;
        std::uint64_t first_rx = 0;
        std::uint64_t last_rx = 0;
        if (shards[0]->split_ab) {
            shards[0]->collect = (shards[0]->polls_done++ & 1) != 0;
        }
        const bool timed = shards[0]->split_ab && n >= 2;
        const std::uint64_t applied_before = timed ? shards[0]->applied : 0;
        const std::uint64_t ab_t0 = timed ? tsc::now() : 0;
        for (int i = 0; i < n; ++i) {
            shards[0]->poll_n = static_cast<std::uint16_t>(n);
            shards[0]->poll_i = static_cast<std::uint16_t>(i);
            shards[0]->poll_at = poll_tsc_always;
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) {
                if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_NO_DESC_TRUNC ||
                    EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_DISCARD) {
                    ++discards;
                }
                continue;
            }
            const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
            const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - prefix;
            std::uint8_t* buf = frames.at(slot);
            if (packets == 0) start = tsc::now();
            ++packets;

            ef_precisetime ts{};
            ef_vi_receive_get_precise_timestamp(vi.get(), buf, &ts);
            const std::uint64_t at =
                static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
            if (first_rx == 0) first_rx = at;
            last_rx = at;
            if (stat_on && first_body == 0) {
                volatile const std::uint8_t* probe = buf + prefix;
                (void)*probe;
                first_body = tsc::now();
            }
            take_packet(shards[0].get(), buf + prefix,
                        static_cast<std::uint32_t>(len), at, &reference,
                        &wanted, trading, &drained);
            const std::size_t give = next_post & (cfg::kRxRingSlots - 1);
            ++next_post;
            if (ef_vi_receive_init(vi.get(), frames.dma(give), give) < 0) ++discards;
        }
        shards[0]->cur_parse = timing ? fenced_now() : 0;
        apply_hits(shards[0].get(), trading);
        if (timed) {
            const std::uint64_t touched = shards[0]->applied - applied_before;
            if (touched != 0) {
                const std::size_t w = shards[0]->collect ? 1 : 0;
                shards[0]->ab_ticks[w] += tsc::now() - ab_t0;
                ++shards[0]->ab_polls[w];
                shards[0]->ab_msgs[w] += touched;
            }
        }
        shards[0]->cur_book = timing ? fenced_now() : 0;
        settle_dirty(shards[0].get(), trading);
        const std::uint64_t done_tsc = tsc::now();
        if (trading) flush_orders(shards[0].get());
        const std::uint64_t sent_tsc = tsc::now();
        if (shards[0]->stat_used < shards[0]->stat_when.size()) {
            const std::size_t k = shards[0]->stat_used++;
            shards[0]->stat_when[k] = shards[0]->poll_when;
            shards[0]->stat_pkts[k] = static_cast<std::uint16_t>(n);
            shards[0]->stat_msgs[k] = static_cast<std::uint16_t>(
                shards[0]->poll_msgs > 65535 ? 65535 : shards[0]->poll_msgs);
            const std::uint64_t span = last_rx > first_rx ? last_rx - first_rx : 0;
            shards[0]->stat_span[k] = static_cast<std::uint32_t>(
                span > 0xffffffffull ? 0xffffffffu : span);
            const std::uint64_t gap = stat_poll_tsc > shards[0]->last_poll_seen
                ? static_cast<std::uint64_t>(
                      (stat_poll_tsc - shards[0]->last_poll_seen) / tps)
                : 0;
            shards[0]->stat_gap[k] = static_cast<std::uint32_t>(
                gap > 0xffffffffull ? 0xffffffffu : gap);
            const std::uint64_t proc = done_tsc > stat_poll_tsc
                ? static_cast<std::uint64_t>((done_tsc - stat_poll_tsc) / tps) : 0;
            shards[0]->stat_proc[k] = static_cast<std::uint32_t>(
                proc > 0xffffffffull ? 0xffffffffu : proc);
            const std::uint64_t snd = sent_tsc > done_tsc
                ? static_cast<std::uint64_t>((sent_tsc - done_tsc) / tps) : 0;
            shards[0]->stat_send[k] = static_cast<std::uint32_t>(
                snd > 0xffffffffull ? 0xffffffffu : snd);
            shards[0]->stat_raw_poll[k] = stat_poll_tsc;
            shards[0]->stat_raw_body[k] = first_body;
            shards[0]->stat_raw_done[k] = done_tsc;
            shards[0]->stat_raw_nic[k] = first_rx;
            const std::uint64_t blind = shards[0]->last_empty != 0 &&
                                        stat_poll_tsc > shards[0]->last_empty
                ? static_cast<std::uint64_t>(
                      (stat_poll_tsc - shards[0]->last_empty) / tps) : 0;
            shards[0]->stat_blind[k] = static_cast<std::uint32_t>(
                blind > 0xffffffffull ? 0xffffffffu : blind);
        }
        shards[0]->last_poll_seen = sent_tsc;
        ef_vi_receive_push(vi.get());
    }
    const double wall = (last_seen - start) / tps / 1e9;

    done.store(true, std::memory_order_release);
    if (ack_thread.joinable()) ack_thread.join();
    for (auto& sh : shards) {
        const std::uint64_t stop = tsc::now() +
            static_cast<std::uint64_t>(2e9 * tsc::ticks_per_ns());
        while (sh->acked_frames < sh->frames && tsc::now() < stop) {
            ef_event evs[8];
            const int got = ef_eventq_poll(ack_vi.get(), evs, 8);
            for (int k = 0; k < got; ++k) {
                if (EF_EVENT_TYPE(evs[k]) != EF_EVENT_TYPE_RX) continue;
                const std::size_t sl = EF_EVENT_RX_RQ_ID(evs[k]);
                const std::size_t ln = EF_EVENT_RX_BYTES(evs[k]) - ack_prefix;
                take_ack(sh.get(), ack_frames.at(sl) + ack_prefix, ln);
                const std::size_t give = ack_post & (kAckRingSlots - 1);
                ++ack_post;
                (void)ef_vi_receive_init(ack_vi.get(), ack_frames.dma(give), give);
            }
            if (got > 0) ef_vi_receive_push(ack_vi.get());
            resend_stale(sh.get(), rto_ticks);
        }
        std::uint8_t* slot = sh->txbuf.at(sh->next_slot);
        const std::size_t n = sh->conn.send(slot, nullptr, 0,
                                            mintcp::kFin | mintcp::kAck);
        (void)ef_vi_transmit(sh->tx.get(), sh->txbuf.dma(sh->next_slot),
                             static_cast<int>(n),
                             static_cast<ef_request_id>(sh->next_slot));
        std::printf("own connection closed, %" PRIu64 " messages resent, "
                    "%" PRIu64 " resets or closes from the far side\n",
                    sh->resends, sh->peer_gone);
    }

    nic::Drops after;
    const bool clean = have_counters && nic::read_drops(opt.intf, &after) && before == after;

    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    std::uint64_t applied_window = 0;
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0, orphan = 0, live = 0;
    std::uint64_t full = 0, bound = 0, unbound = 0;
    std::uint64_t sent = 0, refused = 0, stamped = 0, no_slot = 0;
    std::uint64_t wnd_block = 0;
    std::uint32_t wnd_min = 0xffffffffu, wnd_max = 0;
    std::uint64_t warmed = 0;
    std::uint64_t window_messages = 0, paced_orders = 0;
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
    hist::Hist pooled, warm;
    for (const auto& s : shards) {
        sent += s->sent;
        refused += s->refused;
        stamped += s->stamped;
        no_slot += s->no_slot;
        wnd_block += s->wnd_block;
        if (s->wnd_min < wnd_min) wnd_min = s->wnd_min;
        if (s->wnd_max > wnd_max) wnd_max = s->wnd_max;
        warmed += s->warmed;
        window_messages = s->window_messages;
        paced_orders += s->paced_orders;
        windows = s->windows;
        windows_dropped = s->windows_dropped;
        dropped_samples = s->dropped_samples;
        warm.merge(s->warmup);
        pooled.merge(s->latency);
        messages = s->messages;
        applied += s->applied;
        applied_window += s->applied_window;
        buys += s->buys;
        sells += s->sells;
        gaps += s->gaps;
        duplicates += s->duplicates;
        lapped += s->lapped;
        orphan += s->book.counters().orphan;
        full += s->book.counters().full;
        bound += s->bound;
        unbound += s->unbound;
        live += s->book.live();
    }

    std::printf("packets        %" PRIu64 "\n", packets);
    std::printf("messages       %" PRIu64 " (each shard saw all of them)\n", messages);
    std::printf("applied        %" PRIu64 " across the shards\n", applied);
    std::printf("order rate     %" PRIu64 " stamped over %" PRIu64
                " in-window book touches, %.3f%%\n",
                stamped, applied_window,
                applied_window ? 100.0 * double(stamped) / double(applied_window) : 0.0);
    std::printf("orders alive   %" PRIu64 "\n", live);
    if (shards[0]->split_ab) {
        const double tps = tsc::ticks_per_ns();
        static const char* const path[2] = {"apply as parsed  ", "collect then apply"};
        for (std::size_t w = 0; w < 2; ++w) {
            if (shards[0]->ab_msgs[w] == 0) continue;
            std::printf("split %s %" PRIu64 " polls, %.2f book touches each, "
                        "%.1f ns a touch\n",
                        path[w], shards[0]->ab_polls[w],
                        double(shards[0]->ab_msgs[w]) / double(shards[0]->ab_polls[w]),
                        double(shards[0]->ab_ticks[w]) / tps /
                            double(shards[0]->ab_msgs[w]));
        }
    }
    std::printf("bound          %" PRIu64 " securities, %" PRIu64 " without a price space\n",
                bound, unbound);
    std::printf("rate           %.3f s, %.2f M messages/s\n", wall,
                wall > 0 ? messages / wall / 1e6 : 0.0);
    std::printf("signals        %" PRIu64 " (buy %" PRIu64 ", sell %" PRIu64 ")"
                ", of them in the paced stretch %" PRIu64 "\n",
                buys + sells, buys, sells, paced_orders);
    std::printf("window msgs    %" PRIu64 ", windows %" PRIu64 ", of them thrown away %"
                PRIu64 " (%" PRIu64 " messages)\n",
                window_messages, windows, windows_dropped, dropped_samples);
    std::printf("gaps %" PRIu64 ", duplicates %" PRIu64 ", lapped %" PRIu64
                ", orphan %" PRIu64 ", discards %" PRIu64 ", table full %" PRIu64 "\n",
                gaps, duplicates, lapped, orphan, discards, full);
    if (trading) {
        std::printf("orders sent    %" PRIu64 ", refused %" PRIu64
                    ", no free slot %" PRIu64 ", stamped %" PRIu64 "\n",
                    sent, refused, no_slot, stamped);
        if (wnd_block != 0) {
            std::printf("peer window    blocked %" PRIu64 " orders, window then %u to %u"
                        " bytes\n",
                        wnd_block, wnd_min, wnd_max);
        }
        std::printf("path warmed    %" PRIu64 " times, nothing on the wire\n", warmed);
        std::printf("warm up        %" PRIu64 " samples, thrown away\n", warm.samples());
        std::printf("tick to trade  p50 %" PRIu64 " p99 %" PRIu64 " p99.9 %" PRIu64
                    " p99.99 %" PRIu64 " max %" PRIu64 " ns over %" PRIu64 " samples\n",
                    pooled.quantile(0.5), pooled.quantile(0.99), pooled.quantile(0.999),
                    pooled.quantile(0.9999), pooled.largest(), pooled.samples());
        if (opt.out != nullptr) {
            if (sample::make_dir(opt.out)) {
                std::FILE* w = std::fopen((std::string(opt.out) + "/windows.csv").c_str(), "w");
                if (w != nullptr) {
                    std::fputs("window,first_row,samples,over_1ms,worst_ns\n", w);
                    std::uint64_t row = 0;
                    for (std::size_t k = 1; k <= windows && k < 32768; ++k) {
                        std::uint64_t n = 0, over = 0, worst = 0;
                        for (const auto& s : shards) {
                            n += s->per_window[k].samples;
                            over += s->per_window[k].over_ms;
                            worst = std::max(worst, s->per_window[k].worst);
                        }
                        std::fprintf(w, "%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%u\n",
                                     k, row, n, over, worst);
                        row += n;
                    }
                    std::fclose(w);
                }
            }
            sample::Log all;
            all.reserve(pooled.samples() + 1);
            std::uint64_t lost = 0;
            std::vector<std::pair<std::uint16_t, std::uint64_t>> by_window;
            by_window.reserve(pooled.samples() + 1);
            for (const auto& s : shards) {
                for (std::size_t i = 0; i < s->raw.size() && i < s->raw_window.size(); ++i) {
                    by_window.emplace_back(s->raw_window[i], s->raw.data()[i]);
                }
                lost += s->raw.over();
            }
            std::stable_sort(by_window.begin(), by_window.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& e : by_window) all.add(e.second);
            if (!sample::write_csv(opt.out, "latency.csv", all)) {
                std::fprintf(stderr, "could not write %s/latency.csv\n", opt.out);
            } else {
                std::printf("samples        %s/latency.csv, %zu rows, %" PRIu64
                            " did not fit\n", opt.out, all.size(), lost);
            }
            std::FILE* e = std::fopen((std::string(opt.out) + "/events.csv").c_str(), "w");
            if (e != nullptr) {
                std::fprintf(e, "# ticks_per_ns %.6f\n", tsc::ticks_per_ns());
                std::fputs("window,rx_ns,latency_ns,poll_n,poll_i,sym,"
                           "poll_tsc,body_tsc,parse_tsc,book_tsc,"
                           "in_tsc,out_tsc,fill_tsc,tcp_tsc,ring_tsc\n", e);
                std::uint64_t rows = 0;
                for (const auto& s : shards) {
                    const std::size_t have =
                        std::min(s->raw.size(), s->raw_rx.size());
                    for (std::size_t i = 0; i < have; ++i) {
                        std::fprintf(e,
                                     "%u,%" PRIu64 ",%" PRIu64 ",%u,%u,%u,"
                                     "%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                     ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                     ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                                     s->raw_window[i], s->raw_rx[i], s->raw.data()[i],
                                     s->raw_polln[i], s->raw_polli[i], s->raw_sym[i],
                                     s->raw_poll[i], s->raw_body[i],
                                     s->raw_parse[i], s->raw_book[i], s->raw_in[i],
                                     s->raw_out[i],
                                     s->raw_s1[i], s->raw_s2[i], s->raw_s3[i]);
                        ++rows;
                    }
                }
                std::fclose(e);
                std::printf("with context   %s/events.csv, %" PRIu64 " rows\n", opt.out, rows);
            }
            std::FILE* c = std::fopen((std::string(opt.out) + "/settle.csv").c_str(), "w");
            if (c != nullptr) {
                std::fputs("ms_before_open,latency_ns\n", c);
                std::uint64_t rows = 0;
                for (const auto& s : shards) {
                    for (std::size_t i = 0; i < s->settle_at.size(); ++i) {
                        std::fprintf(c, "%" PRIu64 ",%" PRIu64 "\n",
                                     s->settle_at.data()[i] * 10, s->settle_ns.data()[i]);
                        ++rows;
                    }
                }
                std::fclose(c);
                std::printf("settle curve   %s/settle.csv, %" PRIu64 " rows\n", opt.out, rows);
            }
        }
    }
    {
        std::uint64_t polls = 0, biggest = 0;
        for (int k = 1; k < 65; ++k) {
            polls += shards[0]->depth[k];
            if (shards[0]->depth[k] != 0) biggest = static_cast<std::uint64_t>(k);
        }
        if (polls != 0) {
            const double want[4] = {0.5, 0.9, 0.99, 0.999};
            std::uint64_t at[4] = {};
            std::uint64_t seen = 0;
            int m = 0;
            for (int k = 1; k < 65 && m < 4; ++k) {
                seen += shards[0]->depth[k];
                while (m < 4 && static_cast<double>(seen) >= want[m] * static_cast<double>(polls)) {
                    at[m++] = static_cast<std::uint64_t>(k);
                }
            }
            std::printf("poll depth     p50 %" PRIu64 " p90 %" PRIu64 " p99 %" PRIu64
                        " p99.9 %" PRIu64 " max %" PRIu64 " over %" PRIu64
                        " polls that found something\n",
                        at[0], at[1], at[2], at[3], biggest, polls);
            std::uint64_t packets = 0;
            for (int k = 1; k < 65; ++k) {
                packets += shards[0]->depth[k] * static_cast<std::uint64_t>(k);
            }
            std::uint64_t at_w[4] = {};
            seen = 0;
            m = 0;
            for (int k = 1; k < 65 && m < 4; ++k) {
                seen += shards[0]->depth[k] * static_cast<std::uint64_t>(k);
                while (m < 4 && static_cast<double>(seen) >= want[m] * static_cast<double>(packets)) {
                    at_w[m++] = static_cast<std::uint64_t>(k);
                }
            }
            std::printf("depth by packet p50 %" PRIu64 " p90 %" PRIu64 " p99 %" PRIu64
                        " p99.9 %" PRIu64 " over %" PRIu64
                        " packets, i.e. how deep the poll was that carried a packet\n",
                        at_w[0], at_w[1], at_w[2], at_w[3], packets);
        }
    }
    if (opt.stat != nullptr && shards[0]->stat_used != 0) {
        std::string made;
        for (const char* c = opt.stat;; ++c) {
            if (*c == '/' || *c == 0) {
                if (!made.empty()) (void)::mkdir(made.c_str(), 0755);
                if (*c == 0) break;
            }
            made.push_back(*c);
        }
        const std::string path = std::string(opt.stat) + "/polls_raw.csv";
        std::FILE* sf = std::fopen(path.c_str(), "w");
        if (sf == nullptr) std::perror(path.c_str());
        if (sf != nullptr) {
            std::fprintf(sf,
                         "day_ns,packets,itch_msgs,rx_span_ns,idle_ns,proc_ns,send_ns,"
                         "raw_poll_tsc,raw_body_tsc,raw_done_tsc,raw_nic_ns,blind_ns\n");
            for (std::size_t k = 0; k < shards[0]->stat_used; ++k) {
                std::fprintf(sf,
                             "%" PRIu64 ",%u,%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64
                             ",%" PRIu64 ",%" PRIu64 ",%u\n",
                             shards[0]->stat_when[k], shards[0]->stat_pkts[k],
                             shards[0]->stat_msgs[k], shards[0]->stat_span[k],
                             shards[0]->stat_gap[k], shards[0]->stat_proc[k],
                             shards[0]->stat_send[k], shards[0]->stat_raw_poll[k],
                             shards[0]->stat_raw_body[k], shards[0]->stat_raw_done[k],
                             shards[0]->stat_raw_nic[k], shards[0]->stat_blind[k]);
            }
            std::fclose(sf);
            std::printf("stat rows      %s, %zu polls\n", path.c_str(),
                        shards[0]->stat_used);
        }
    }
    if (stat_on) {
        static const char* what[3] = {"drained.fetch_add", "reap", "resend_stale"};
        for (int q = 0; q < 3; ++q) {
            std::printf("empty path     %-18s worst %" PRIu64 " ns, over 10us %" PRIu64
                        " times\n", what[q], shards[0]->empty_worst[q],
                        shards[0]->empty_over[q]);
        }
    }
    std::printf("card counters  %s\n",
                !have_counters ? "could not be read" : (clean ? "unchanged" : "MOVED"));
    if (have_counters && !clean) (void)nic::report_drops(before, after);
    return (gaps == 0 && lapped == 0 && orphan == 0 && discards == 0 && full == 0 &&
            windows_dropped == 0 && (clean || !have_counters))
               ? 0
               : 1;
}
