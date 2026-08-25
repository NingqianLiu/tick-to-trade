#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <malloc.h>
#include <sys/mman.h>
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
#include "net/ouch.hpp"
#include "net/ring.hpp"

namespace {

struct Options {
    const char* intf = "enp129s0f1";
    const char* reference = "results/prev_close_061226.csv";
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    std::uint16_t dst_port = 26477;
    std::uint16_t dst_port_b = 0;
    unsigned shards = 3;
    int cpu_base = 4;
    std::uint32_t threshold = 88;
    std::uint64_t idle_ms = 2000;
    const char* order_ip = nullptr;
    std::uint16_t order_port = 45100;
    const char* out = nullptr;
    bool profile = false;
    bool lock_memory = false;
};

constexpr std::size_t kOrderSlots = 64;
constexpr int kHeaderRoom = 128;

struct Shard {
    Shard(std::size_t orders, std::size_t level_words, std::uint32_t pct,
          const win::Params& wp)
        : book(orders, level_words), signal(pct), phase(wp), symbols(65536 * 8, ' ') {}

    book::OrderBook book;
    book::Imbalance signal;
    win::Tracker phase;
    ring::Cursor cursor;
    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0;
    std::uint64_t expect_seq = 0;
    bool started = false;

    int fd = -1;
    ef::Vi tx;
    ef::Frames txbuf;
    onload_delegated_send ds{};
    bool ready = false;
    std::size_t next_slot = 0;
    std::uint32_t user_ref = 1;
    std::uint64_t sent = 0, refused = 0, stamped = 0, reaped = 0, no_slot = 0;
    std::uint64_t why[8] = {};
    ef_vi_tx_warm_state warm_state{};
    std::uint64_t since_send = 0, warmed = 0;
    std::uint64_t at_slot[kOrderSlots] = {};
    bool counted[kOrderSlots] = {};
    std::uint16_t before[kOrderSlots] = {};
    sample::Log settle_at, settle_ns;
    hist::Hist latency;
    hist::Hist warmup;
    hist::Hist stage[5];
    bool profile = false;
    std::uint64_t window_messages = 0, paced_orders = 0;
    win::Phase was = win::Phase::kGap;
    std::uint64_t drain_mark = 0;
    bool caught_up = false, window_ok = false;
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
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
    std::vector<std::uint8_t> symbols;
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

bool open_order_path(Shard* self, const char* ip, std::uint16_t port,
                     const char* intf) {
    self->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::connect(self->fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::perror("connect");
        return false;
    }
    int on = 1;
    ::setsockopt(self->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    if (!self->tx.open(intf, 0, 511,
                       static_cast<enum ef_vi_flags>(EF_VI_TX_TIMESTAMPS |
                                                     EF_VI_TX_PUSH_ALWAYS))) {
        return false;
    }
    if (!self->txbuf.alloc(self->tx, kOrderSlots, cfg::kOrderSlotBytes)) return false;
    for (std::size_t i = 0; i < kOrderSlots; ++i) {
        ouch::prefill(self->txbuf.at(i) + kHeaderRoom);
    }
    return true;
}

void arm(Shard* self) {
    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    self->ds.headers = slot;
    self->ds.headers_len = kHeaderRoom;
    const enum onload_delegated_send_rc rc = onload_delegated_send_prepare(
        self->fd, static_cast<int>(ouch::kOrderPacketLen),
        ONLOAD_DELEGATED_SEND_FLAG_RESOLVE_ARP, &self->ds);
    self->ready = rc == ONLOAD_DELEGATED_SEND_RC_OK;
    if (!self->ready) ++self->why[static_cast<int>(rc) & 7];
}

void reap(Shard* self);

void send_order(Shard* self, std::uint16_t sym, char side, std::uint64_t rx_ts,
                bool keep, std::uint16_t before) {
    if (self->sent - self->reaped >= kOrderSlots) {
        reap(self);
        if (self->sent - self->reaped >= kOrderSlots) {
            ++self->no_slot;
            return;
        }
    }
    if (!self->ready) {
        ++self->refused;
        arm(self);
        return;
    }
    std::uint32_t price = 0, shares = 0;
    const std::uint8_t take_from =
        side == ouch::kBuy ? book::PriceLevels::kSell : book::PriceLevels::kBuy;
    if (!self->book.best(sym, take_from, &price, &shares)) return;

    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    const std::uint64_t s0 = self->profile ? tsc::now() : 0;
    std::uint8_t* msg = slot + self->ds.headers_len;
    if (msg != slot + kHeaderRoom) {
        std::memcpy(msg, slot + kHeaderRoom, ouch::kOrderPacketLen);
    }
    ouch::fill(msg, self->user_ref, side, 1, &self->symbols[sym * 8], price);
    ouch::set_cl_ord_id(msg, self->user_ref);
    ++self->user_ref;
    const std::uint64_t s1 = self->profile ? tsc::now() : 0;
    onload_delegated_send_tcp_update(&self->ds, static_cast<int>(ouch::kOrderPacketLen), 1);

    const std::size_t frame = static_cast<std::size_t>(self->ds.headers_len) +
                              ouch::kOrderPacketLen;
    const std::size_t id = self->next_slot;
    self->at_slot[id] = rx_ts;
    self->counted[id] = keep;
    self->before[id] = before;
    if (ef_vi_transmit(self->tx.get(), self->txbuf.dma(id), static_cast<int>(frame),
                       static_cast<ef_request_id>(id)) < 0) {
        ++self->refused;
        return;
    }
    ++self->sent;
    if (self->profile) {
        const std::uint64_t s2 = tsc::now();
        self->stage[2].add(s1 - s0);
        self->stage[3].add(s2 - s1);
    }
    iovec iov{msg, ouch::kOrderPacketLen};
    (void)onload_delegated_send_complete(self->fd, &iov, 1, 0);
    self->next_slot = (self->next_slot + 1) % kOrderSlots;
    self->since_send = 0;
    arm(self);
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
        const std::uint64_t in = self->at_slot[id];
        if (in != 0 && out > in) {
            (self->counted[id] ? self->latency : self->warmup).add(out - in);
            if (!self->counted[id] && self->before[id] != 0) {
                self->settle_at.add(self->before[id]);
                self->settle_ns.add(out - in);
            }
            if (self->counted[id]) {
                self->raw.add(out - in);
                if (self->raw_window.size() < self->raw_window.capacity()) {
                    self->raw_window.push_back(static_cast<std::uint16_t>(self->at_window));
                }
                if (self->at_window < self->per_window.size()) {
                    Shard::WindowStat& w = self->per_window[self->at_window];
                    ++w.samples;
                    if (out - in > 1000000) ++w.over_ms;
                    if (out - in > w.worst) w.worst = out - in;
                }
            }
        }
        self->at_slot[id] = 0;
        ++self->stamped;
        ++self->reaped;
    }
}

void take_packet(Shard* self, const std::uint8_t* buf, std::uint32_t len,
                 std::uint64_t hw_ts, unsigned id, unsigned shards,
                 const std::unordered_map<std::string, std::uint32_t>* reference,
                 bool trading, const std::atomic<std::uint64_t>* drained) {
    const bool by_mask = (shards & (shards - 1)) == 0;
    const unsigned mask = shards - 1;
    const auto mine = [=](std::uint16_t sym) {
        return by_mask ? (sym & mask) == id : sym % shards == id;
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
                win::note_session(m, &self->phase);
                const win::Phase where = self->phase.advance(m.timestamp());
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
                if (m.type() == 'R' && mine(sym)) {
                    const char* s2 =
                        reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                    std::memcpy(&self->symbols[sym * 8], s2, itch::kStockSymbolLen);
                    std::size_t n = itch::kStockSymbolLen;
                    while (n > 0 && s2[n - 1] == ' ') --n;
                    const auto it = reference->find(std::string(s2, n));
                    if (it != reference->end()) self->book.bind(sym, it->second);
                    return true;
                }
                if (!mine(sym)) return true;
                std::uint16_t touched = 0;
                const std::uint64_t t0 = self->profile ? tsc::now() : 0;
                if (!self->book.apply(m, &touched)) return true;
                ++self->applied;
                const std::uint64_t t1 = self->profile ? tsc::now() : 0;
                if (trading && ++self->since_send >= 256) {
                    warm(self);
                    self->since_send = 0;
                }
                const auto what =
                    self->signal.check(self->book.top3(touched, book::PriceLevels::kBuy),
                                       self->book.top3(touched, book::PriceLevels::kSell));
                const std::uint64_t t2 = self->profile ? tsc::now() : 0;
                if (self->profile) {
                    self->stage[0].add(t1 - t0);
                    self->stage[1].add(t2 - t1);
                }
                if (what == book::Imbalance::Signal::kNone) return true;
                const char side =
                    what == book::Imbalance::Signal::kBuy ? ouch::kBuy : ouch::kSell;
                if (side == ouch::kBuy) ++self->buys; else ++self->sells;
                if (!paced) return true;
                ++self->paced_orders;
                const std::uint16_t before =
                    where == win::Phase::kSettle
                        ? static_cast<std::uint16_t>(
                              (self->phase.open() - m.timestamp()) / 10000000 + 1)
                        : 0;
                if (trading) send_order(self, touched, side, hw_ts, measured, before);
                return true;
            });
    }
}

void run_shard(Shard* self, unsigned id, unsigned shards, const ring::Ring* r,
               const std::atomic<bool>* done, int cpu,
               const std::unordered_map<std::string, std::uint32_t>* reference,
               bool trading, const std::atomic<std::uint64_t>* drained) {
    pin(cpu);
    if (trading) arm(self);
    ring::View v;
    while (true) {
        const ring::Ring::State s = r->take(self->cursor.want, &v);
        if (s == ring::Ring::State::kWaiting) {
            self->caught_up = true;
            if (trading) reap(self);
            if (done->load(std::memory_order_acquire) &&
                self->cursor.want > r->published()) {
                if (trading) reap(self);
                return;
            }
            continue;
        }
        if (s == ring::Ring::State::kLapped) {
            ++self->lapped;
            self->cursor.want = r->published();
            continue;
        }
        ++self->cursor.want;
        take_packet(self, v.buf, v.len, v.hw_ts, id, shards, reference, trading, drained);
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
        else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            if (!parse_ip(argv[++i], &opt.dst_ip)) return 2;
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            opt.dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--dst-port-b") == 0 && has) {
            opt.dst_port_b = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--shards") == 0 && has) {
            opt.shards = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--cpu-base") == 0 && has) {
            opt.cpu_base = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--threshold") == 0 && has) {
            opt.threshold = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--idle-ms") == 0 && has) {
            opt.idle_ms = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--order-ip") == 0 && has) {
            opt.order_ip = argv[++i];
        } else if (std::strcmp(a, "--order-port") == 0 && has) {
            opt.order_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--out") == 0 && has) {
            opt.out = argv[++i];
        } else if (std::strcmp(a, "--profile") == 0) {
            opt.profile = true;
        } else if (std::strcmp(a, "--lock-memory") == 0) {
            opt.lock_memory = true;
        } else {
            std::fprintf(stderr,
                         "usage: trader [--intf I] [--reference CSV] [--dst-ip A.B.C.D]\n"
                         "              [--dst-port N] [--dst-port-b N] [--shards N]\n"
                         "              [--cpu-base N] [--threshold N] [--idle-ms N]\n"
                         "              [--order-ip A.B.C.D] [--order-port N] [--out DIR]\n"
                         "  ITCH_SKIP_WINDOWS / ITCH_MAX_WINDOWS pick which windows count\n"
                         "  sending orders needs onload in front of it\n");
            return 2;
        }
    }
    if (opt.shards == 0) opt.shards = 1;

    std::unordered_map<std::string, std::uint32_t> reference;
    std::vector<std::uint32_t> prices;
    if (!read_reference(opt.reference, &reference, &prices)) {
        std::fprintf(stderr, "cannot read %s\n", opt.reference);
        return 1;
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

    const std::size_t prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(vi.get()));
    std::size_t next_post = 0;
    for (; next_post < cfg::kRxDescriptors - 1; ++next_post) {
        if (ef_vi_receive_init(vi.get(), frames.dma(next_post), next_post) < 0) return 1;
    }
    ef_vi_receive_push(vi.get());

    const std::size_t orders_each = 12u << 20;
    const std::size_t words_each =
        book::PriceLevels::budget_for(prices) / opt.shards * 3 / 2 + (1u << 20);
    std::printf("%u shards, %.2f GB of prices and %zu orders each\n", opt.shards,
                words_each * 8.0 / 1e9, orders_each);

    ring::Ring r(cfg::kRxRingSlots);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> drained{0};
    std::vector<std::unique_ptr<Shard>> shards;
    std::vector<std::thread> threads;
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
    for (unsigned i = 0; i < opt.shards; ++i) {
        shards.push_back(
            std::make_unique<Shard>(orders_each, words_each, opt.threshold, wp));
        shards.back()->skip = skip;
        shards.back()->keep = keep;
        shards.back()->raw.reserve(sample_cap);
        shards.back()->raw_window.assign(sample_cap, 0);
        shards.back()->raw_window.clear();
        shards.back()->settle_at.reserve(4u << 20);
        shards.back()->settle_ns.reserve(4u << 20);
        shards.back()->profile = opt.profile;
    }
    if (skip != 0 || keep != 0) {
        std::printf("counting windows %llu..%s\n",
                    static_cast<unsigned long long>(skip + 1),
                    keep == 0 ? "end"
                              : std::to_string(skip + keep).c_str());
    }
    const bool trading = opt.order_ip != nullptr;
    for (unsigned i = 0; i < opt.shards; ++i) {
        if (!trading) continue;
        if (!open_order_path(shards[i].get(), opt.order_ip,
                             static_cast<std::uint16_t>(opt.order_port + i), opt.intf)) {
            std::fprintf(stderr, "shard %u could not open its order path\n", i);
            return 1;
        }
    }
    const bool single = opt.shards == 1;
    for (unsigned i = 0; !single && i < opt.shards; ++i) {
        threads.emplace_back(run_shard, shards[i].get(), i, opt.shards, &r, &done,
                             opt.cpu_base < 0 ? -1 : opt.cpu_base + 1 + static_cast<int>(i),
                             &reference, trading, &drained);
    }

    nic::Drops before;
    const bool have_counters = nic::read_drops(opt.intf, &before);

    if (single && trading) arm(shards[0].get());
    const double tps = tsc::ticks_per_ns();
    const std::uint64_t idle_ticks = static_cast<std::uint64_t>(opt.idle_ms * 1e6 * tps);
    std::uint64_t last_seen = tsc::now(), start = 0;
    std::uint64_t packets = 0, discards = 0;
    ef_event evs[64];

    for (;;) {
        const int n = ef_eventq_poll(vi.get(), evs, 64);
        if (n == 0) {
            drained.fetch_add(1, std::memory_order_relaxed);
            if (single) {
                shards[0]->caught_up = true;
                if (trading) reap(shards[0].get());
            }
            if (packets != 0 && tsc::now() - last_seen > idle_ticks) break;
            if (packets == 0 && tsc::now() - last_seen > idle_ticks * 15) {
                std::fprintf(stderr, "nothing arrived\n");
                done.store(true, std::memory_order_release);
                for (auto& t : threads) t.join();
                return 1;
            }
            continue;
        }
        last_seen = tsc::now();
        for (int i = 0; i < n; ++i) {
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
            if (single) {
                take_packet(shards[0].get(), buf + prefix,
                            static_cast<std::uint32_t>(len), at, 0, 1, &reference,
                            trading, &drained);
            } else {
                r.publish(buf + prefix, static_cast<std::uint32_t>(len), at, ts.tv_flags);
            }
            const std::size_t give = next_post & (cfg::kRxRingSlots - 1);
            ++next_post;
            if (ef_vi_receive_init(vi.get(), frames.dma(give), give) < 0) ++discards;
        }
        ef_vi_receive_push(vi.get());
    }
    const double wall = (last_seen - start) / tps / 1e9;

    done.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    nic::Drops after;
    const bool clean = have_counters && nic::read_drops(opt.intf, &after) && before == after;

    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0, orphan = 0, live = 0;
    std::uint64_t full = 0;
    std::uint64_t sent = 0, refused = 0, stamped = 0, no_slot = 0;
    std::uint64_t why[8] = {};
    std::uint64_t warmed = 0;
    std::uint64_t window_messages = 0, paced_orders = 0;
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
    hist::Hist pooled, warm;
    for (const auto& s : shards) {
        sent += s->sent;
        refused += s->refused;
        stamped += s->stamped;
        no_slot += s->no_slot;
        warmed += s->warmed;
        window_messages = s->window_messages;
        paced_orders += s->paced_orders;
        windows = s->windows;
        windows_dropped = s->windows_dropped;
        dropped_samples = s->dropped_samples;
        warm.merge(s->warmup);
        for (int w = 0; w < 8; ++w) why[w] += s->why[w];
        pooled.merge(s->latency);
        messages = s->messages;
        applied += s->applied;
        buys += s->buys;
        sells += s->sells;
        gaps += s->gaps;
        duplicates += s->duplicates;
        lapped += s->lapped;
        orphan += s->book.counters().orphan;
        full += s->book.counters().full;
        live += s->book.live();
    }

    std::printf("packets        %" PRIu64 "\n", packets);
    std::printf("messages       %" PRIu64 " (each shard saw all of them)\n", messages);
    std::printf("applied        %" PRIu64 " across the shards\n", applied);
    std::printf("orders alive   %" PRIu64 "\n", live);
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
        static const char* reason[] = {"ok", "not an onload socket", "headers too small",
                                       "send queue busy", "send window closed",
                                       "no arp", "congestion window closed", "?"};
        std::printf("orders sent    %" PRIu64 ", refused %" PRIu64
                    ", no free slot %" PRIu64 ", stamped %" PRIu64 "\n",
                    sent, refused, no_slot, stamped);
        std::printf("path warmed    %" PRIu64 " times, nothing on the wire\n", warmed);
        for (int w = 1; w < 8; ++w) {
            if (why[w] != 0) std::printf("  prepare said  %-24s %" PRIu64 "\n", reason[w], why[w]);
        }
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
                        std::fprintf(w, "%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
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
    if (opt.profile) {
        static const char* stage_name[4] = {"book update", "strategy", "fill order",
                                            "hand to card"};
        hist::Hist st[4];
        for (const auto& s : shards) for (int k = 0; k < 4; ++k) st[k].merge(s->stage[k]);
        const double tps = tsc::ticks_per_ns();
        for (int k = 0; k < 4; ++k) {
            std::printf("  %-13s p50 %6.0f  p99 %8.0f ns over %" PRIu64 "\n", stage_name[k],
                        st[k].quantile(0.5) / tps, st[k].quantile(0.99) / tps,
                        st[k].samples());
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
