#include <arpa/inet.h>
#include <netinet/in.h>

#include <etherfabric/ef_vi.h>
#include <etherfabric/vi.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/hist.hpp"
#include "common/settings.hpp"
#include "common/tsc.hpp"
#include "net/ef.hpp"
#include "net/mintcp.hpp"
#include "net/ouch.hpp"

namespace {

constexpr std::size_t kSlots = 2048;
constexpr std::size_t kRxSlots = 256;
constexpr std::size_t kSlotBytes = 512;
constexpr std::uint16_t kEtherArp = 0x0806;

struct Side {
    ef::Vi vi;
    ef::Frames tx;
    ef::Frames rx;
    std::size_t next_tx = 0;
    std::size_t next_rx = 0;
    std::size_t prefix = 0;
};

bool send_now(Side& s, std::size_t len) {
    const std::size_t id = s.next_tx;
    s.next_tx = (s.next_tx + 1) % kSlots;
    int rc;
    while ((rc = ef_vi_transmit(s.vi.get(), s.tx.dma(id), static_cast<int>(len),
                                static_cast<ef_request_id>(id))) == -EAGAIN) {
    }
    return rc >= 0;
}

const std::uint8_t* recv_within(Side& s, std::uint64_t ticks, std::size_t* len_out) {
    const std::uint64_t stop = tsc::now() + ticks;
    while (tsc::now() < stop) {
        ef_event evs[8];
        const int n = ef_eventq_poll(s.vi.get(), evs, 8);
        const std::uint8_t* first = nullptr;
        for (int i = 0; i < n; ++i) {
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
            const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
            if (first == nullptr) {
                *len_out = EF_EVENT_RX_BYTES(evs[i]) - s.prefix;
                first = s.rx.at(slot) + s.prefix;
            }
            const std::size_t give = s.next_rx % kRxSlots;
            ++s.next_rx;
            (void)ef_vi_receive_init(s.vi.get(), s.rx.dma(give), give);
        }
        if (n > 0) ef_vi_receive_push(s.vi.get());
        if (first != nullptr) return first;
    }
    return nullptr;
}

bool from_neighbours(std::uint32_t peer_ip, std::uint8_t out[6]) {
    std::FILE* f = std::fopen("/proc/net/arp", "r");
    if (f == nullptr) return false;
    char line[256];
    if (std::fgets(line, sizeof(line), f) == nullptr) { std::fclose(f); return false; }
    bool found = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned a, b, c, d;
        unsigned m[6];
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

bool resolve(Side& s, const std::uint8_t our_mac[6], std::uint32_t our_ip,
             std::uint32_t peer_ip, std::uint8_t out[6]) {
    std::uint8_t* f = s.tx.at(s.next_tx);
    std::memset(f, 0, 60);
    std::memset(f, 0xff, 6);
    std::memcpy(f + 6, our_mac, 6);
    mintcp::put16(f + 12, kEtherArp);
    mintcp::put16(f + 14, 1);
    mintcp::put16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    mintcp::put16(f + 20, 1);
    std::memcpy(f + 22, our_mac, 6);
    mintcp::put32(f + 28, our_ip);
    std::memset(f + 32, 0, 6);
    mintcp::put32(f + 38, peer_ip);
    const double tps = tsc::ticks_per_ns();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!send_now(s, 60)) return false;
        const std::uint64_t wait = static_cast<std::uint64_t>(50e6 * tps);
        std::size_t len = 0;
        const std::uint8_t* p = recv_within(s, wait, &len);
        if (p == nullptr || len < 42) continue;
        if (mintcp::get16(p + 12) != kEtherArp) continue;
        if (mintcp::get16(p + 20) != 2) continue;
        if (mintcp::get32(p + 28) != peer_ip) continue;
        std::memcpy(out, p + 22, 6);
        return true;
    }
    return false;
}

struct Outstanding {
    std::uint32_t seq = 0;
    std::uint32_t end = 0;
    std::size_t slot = 0;
    std::size_t len = 0;
    std::uint64_t at = 0;
};

int pump(Side& s, mintcp::Conn& c, std::uint64_t* acks) {
    ef_event evs[16];
    const int n = ef_eventq_poll(s.vi.get(), evs, 16);
    for (int i = 0; i < n; ++i) {
        if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
        const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
        const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - s.prefix;
        const std::uint8_t* r = s.rx.at(slot) + s.prefix;
        if (len >= mintcp::kHeaderLen &&
            (r[mintcp::kTcpFlagsOff] & mintcp::kAck) != 0) {
            (void)c.on_ack(mintcp::get32(r + mintcp::kTcpAckOff),
                           mintcp::get16(r + mintcp::kTcpWinOff));
            ++*acks;
        }
        const std::size_t give = s.next_rx % kRxSlots;
        ++s.next_rx;
        (void)ef_vi_receive_init(s.vi.get(), s.rx.dma(give), give);
    }
    if (n > 0) ef_vi_receive_push(s.vi.get());
    return n;
}

int send_side(std::uint32_t peer_ip, std::uint16_t peer_port, const char* intf,
              std::uint32_t our_ip, std::uint64_t count) {
    Side s;
    if (!s.vi.open(intf, 511, 511, EF_VI_TX_TIMESTAMPS)) return 1;
    if (!s.tx.alloc(s.vi, kSlots, kSlotBytes)) return 1;
    if (!s.rx.alloc(s.vi, kRxSlots, kSlotBytes)) return 1;
    ef_vi_receive_set_buffer_len(s.vi.get(), kSlotBytes);
    s.prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(s.vi.get()));
    for (; s.next_rx < kRxSlots - 1; ++s.next_rx) {
        if (ef_vi_receive_init(s.vi.get(), s.rx.dma(s.next_rx), s.next_rx) < 0) {
            std::fprintf(stderr, "the card would not take receive buffer %zu\n",
                         s.next_rx);
            return 1;
        }
    }
    ef_vi_receive_push(s.vi.get());

    std::uint8_t our_mac[6];
    if (ef_vi_get_mac(s.vi.get(), s.vi.dh(), our_mac) < 0) {
        std::fprintf(stderr, "cannot read this port's address\n");
        return 1;
    }
    const std::uint16_t our_port = 51000;

    ef_filter_cookie cookie;
    {
        ef_filter_spec fs;
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        if (ef_filter_spec_set_eth_type(&fs, htons(kEtherArp)) < 0 ||
            ef_vi_filter_add(s.vi.get(), s.vi.dh(), &fs, &cookie) < 0) {
            std::fprintf(stderr, "cannot listen for address replies\n");
            return 1;
        }
    }
    {
        ef_filter_spec fs;
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        if (ef_filter_spec_set_ip4_full(&fs, IPPROTO_TCP, htonl(our_ip),
                                        static_cast<int>(htons(our_port)),
                                        htonl(peer_ip),
                                        static_cast<int>(htons(peer_port))) < 0 ||
            ef_vi_filter_add(s.vi.get(), s.vi.dh(), &fs, &cookie) < 0) {
            std::fprintf(stderr, "cannot listen for this connection\n");
            return 1;
        }
    }

    std::uint8_t peer_mac[6];
    if (!from_neighbours(peer_ip, peer_mac) &&
        !resolve(s, our_mac, our_ip, peer_ip, peer_mac)) {
        std::fprintf(stderr, "nobody answered for 10.9.9.%u\n", peer_ip & 0xff);
        return 1;
    }
    std::printf("peer is %02x:%02x:%02x:%02x:%02x:%02x\n", peer_mac[0], peer_mac[1],
                peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);

    eth::Endpoint us{}, them{};
    std::memcpy(us.mac, our_mac, 6);
    us.ip = our_ip;
    us.port = our_port;
    them.ip = peer_ip;
    them.port = peer_port;

    mintcp::Conn c;
    const std::uint32_t isn = static_cast<std::uint32_t>(tsc::now());
    c.open(us, them, peer_mac, isn, 65535);

    const double tps = tsc::ticks_per_ns();
    const std::uint64_t second = static_cast<std::uint64_t>(1e9 * tps);

    std::size_t n = c.send(s.tx.at(s.next_tx), nullptr, 0, mintcp::kSyn);
    if (!send_now(s, n)) return 1;
    std::size_t len = 0;
    const std::uint8_t* p = recv_within(s, second * 2, &len);
    if (p == nullptr || len < mintcp::kHeaderLen) {
        std::fprintf(stderr, "no answer to the open\n");
        return 1;
    }
    const std::uint8_t flags = p[mintcp::kTcpFlagsOff];
    if ((flags & mintcp::kSyn) == 0 || (flags & mintcp::kAck) == 0) {
        std::fprintf(stderr, "the answer was not an open: flags %02x\n", flags);
        return 1;
    }
    c.set_rcv_nxt(mintcp::get32(p + mintcp::kTcpSeqOff) + 1);
    (void)c.on_ack(mintcp::get32(p + mintcp::kTcpAckOff),
                   mintcp::get16(p + mintcp::kTcpWinOff));
    n = c.send(s.tx.at(s.next_tx), nullptr, 0, mintcp::kAck);
    if (!send_now(s, n)) return 1;
    std::printf("open done, they start at %u\n", c.rcv_nxt());

    std::uint8_t body[ouch::kOrderPacketLen];
    ouch::prefill(body);
    const std::uint8_t sym[8] = {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '};

    hist::Hist cost, build_cost, ring_cost;
    std::uint64_t waits = 0, wait_spins = 0;
    std::uint64_t sent = 0, acks = 0, arrived = 0, resends = 0;
    std::vector<Outstanding> out(kSlots);
    std::size_t out_head = 0, out_tail = 0;
    const auto out_size = [&]() { return out_tail - out_head; };
    const std::uint64_t rto = static_cast<std::uint64_t>(1e6 * tps);
    const auto forget_acked = [&](std::uint32_t una) {
        while (out_head != out_tail &&
               mintcp::before_eq(out[out_head % kSlots].end, una)) {
            ++out_head;
        }
    };
    const auto resend_stale = [&]() {
        if (out_head == out_tail) return;
        Outstanding& o = out[out_head % kSlots];
        if (tsc::now() - o.at < rto) return;
        int rc;
        while ((rc = ef_vi_transmit(s.vi.get(), s.tx.dma(o.slot),
                                    static_cast<int>(o.len),
                                    static_cast<ef_request_id>(o.slot))) ==
               -EAGAIN) {
        }
        o.at = tsc::now();
        ++resends;
    };
    for (std::uint64_t i = 0; i < count; ++i) {
        if (c.in_flight() + ouch::kOrderPacketLen > c.peer_wnd() ||
            out_size() + 8 >= kSlots) {
            const std::uint64_t give_up_here = tsc::now() + second * 2;
            while (c.in_flight() + ouch::kOrderPacketLen > c.peer_wnd() ||
                   out_size() + 8 >= kSlots) {
                arrived += static_cast<std::uint64_t>(pump(s, c, &acks));
                forget_acked(c.snd_una());
                resend_stale();
                if (tsc::now() > give_up_here) {
                    std::fprintf(stderr,
                                 "stalled after %" PRIu64 " orders: %u bytes out, "
                                 "window %u, %" PRIu64 " acknowledgements seen, "
                                 "%" PRIu64 " packets arrived\n",
                                 i, c.in_flight(), c.peer_wnd(), acks, arrived);
                    return 1;
                }
            }
        }
        ouch::fill(body, static_cast<std::uint32_t>(i + 1), ouch::kBuy, 1, sym,
                   1000000);
        ouch::set_cl_ord_id(body, i + 1);
        const std::uint64_t t0 = tsc::now();
        const std::size_t frame =
            c.send(s.tx.at(s.next_tx), body, ouch::kOrderPacketLen,
                   mintcp::kAck | mintcp::kPsh);
        const std::size_t id = s.next_tx;
        s.next_tx = (s.next_tx + 1) % kSlots;
        const std::uint64_t tb = tsc::now();
        int rc;
        int spins = 0;
        while ((rc = ef_vi_transmit(s.vi.get(), s.tx.dma(id),
                                    static_cast<int>(frame),
                                    static_cast<ef_request_id>(id))) == -EAGAIN) {
            ++spins;
        }
        const std::uint64_t t1 = tsc::now();
        build_cost.add(tb - t0);
        ring_cost.add(t1 - tb);
        if (spins != 0) { ++waits; wait_spins += static_cast<std::uint64_t>(spins); }
        if (rc < 0) {
            std::fprintf(stderr, "the card refused a frame: %d\n", rc);
            return 1;
        }
        cost.add(t1 - t0);
        ++sent;
        Outstanding o;
        o.seq = c.snd_nxt() - static_cast<std::uint32_t>(ouch::kOrderPacketLen);
        o.end = c.snd_nxt();
        o.slot = id;
        o.len = frame;
        o.at = t1;
        out[out_tail % kSlots] = o;
        ++out_tail;
        arrived += static_cast<std::uint64_t>(pump(s, c, &acks));
        forget_acked(c.snd_una());
        resend_stale();
    }

    const std::uint64_t give_up = tsc::now() + second * 5;
    while (c.in_flight() != 0 && tsc::now() < give_up) {
        (void)pump(s, c, &acks);
        forget_acked(c.snd_una());
        resend_stale();
    }
    n = c.send(s.tx.at(s.next_tx), nullptr, 0, mintcp::kFin | mintcp::kAck);
    (void)send_now(s, n);

    const double per = tps;
    std::printf("resent %" PRIu64 " segments\n", resends);
    std::printf("sent %" PRIu64 " orders, %" PRIu64 " acknowledgements seen, "
                "%" PRIu32 " bytes still unacknowledged\n",
                sent, acks, c.in_flight());
    std::printf("  laying the frame out  p50 %.0f p99 %.0f p99.9 %.0f max %.0f ns\n",
                build_cost.quantile(0.5) / per, build_cost.quantile(0.99) / per,
                build_cost.quantile(0.999) / per,
                static_cast<double>(build_cost.largest()) / per);
    std::printf("  ringing the card      p50 %.0f p99 %.0f p99.9 %.0f max %.0f ns\n",
                ring_cost.quantile(0.5) / per, ring_cost.quantile(0.99) / per,
                ring_cost.quantile(0.999) / per,
                static_cast<double>(ring_cost.largest()) / per);
    std::printf("  the card's queue was full %" PRIu64 " times, %" PRIu64 " spins\n",
                waits, wait_spins);
    std::printf("one send costs p50 %.0f p90 %.0f p99 %.0f p99.9 %.0f max %.0f ns\n",
                cost.quantile(0.5) / per, cost.quantile(0.9) / per,
                cost.quantile(0.99) / per, cost.quantile(0.999) / per,
                static_cast<double>(cost.largest()) / per);
    return c.in_flight() == 0 ? 0 : 1;
}

}

int main(int argc, char** argv) {
    if (argc >= 6 && std::strcmp(argv[1], "--send") == 0) {
        std::uint32_t peer = 0, local = 0;
        unsigned a, b, cc, d;
        if (std::sscanf(argv[2], "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 2;
        peer = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                         static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(d));
        if (std::sscanf(argv[5], "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 2;
        local = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                          static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(d));
        const std::uint64_t count = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) : 1000;
        return send_side(peer, static_cast<std::uint16_t>(std::atoi(argv[3])), argv[4],
                         local, count);
    }
    std::fprintf(stderr,
                 "usage: tcp_check --send <peer ip> <peer port> <interface>"
                 " <local ip> [count]\n");
    return 2;
}
