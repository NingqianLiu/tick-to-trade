#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <etherfabric/ef_vi.h>
#include <etherfabric/vi.h>
#include <onload/extensions.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "common/settings.hpp"
#include "common/tsc.hpp"
#include "net/ef.hpp"

namespace {

constexpr int kHeaderRoom = 128;
constexpr std::size_t kSlots = 64;
constexpr std::size_t kSlotBytes = 2048;

int listen_side(const char* ip, std::uint16_t port) {
    const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::perror("bind");
        return 1;
    }
    ::listen(srv, 1);
    std::printf("listening on %s:%u\n", ip, port);
    std::fflush(stdout);

    const int c = ::accept(srv, nullptr, nullptr);
    if (c < 0) {
        std::perror("accept");
        return 1;
    }
    char buf[65536];
    std::size_t total = 0, reads = 0;
    std::size_t msgs = 0, bad = 0, held = 0;
    unsigned char pending[4096];
    for (;;) {
        const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        total += static_cast<std::size_t>(n);
        if (reads == 0) {
            std::printf("first bytes:");
            for (ssize_t k = 0; k < n && k < 24; ++k)
                std::printf(" %02x", static_cast<unsigned char>(buf[k]));
            std::printf("\n");
        }
        ++reads;
        for (ssize_t i = 0; i < n; ++i) {
            if (held < sizeof(pending)) pending[held++] = static_cast<unsigned char>(buf[i]);
            if (held < 2) continue;
            const std::size_t want =
                2 + (static_cast<std::size_t>(pending[0]) << 8) + pending[1];
            if (want < 4 || want > sizeof(pending)) { ++bad; held = 0; continue; }
            if (held < want) continue;
            if (pending[2] == 'U' && pending[3] == 'O') ++msgs; else ++bad;
            held = 0;
        }
    }
    std::printf("%zu reads\n", reads);
    std::printf("connection closed after %zu bytes\n", total);
    std::printf("orders received %zu, unrecognised %zu, %zu bytes left over\n",
                msgs, bad, held);
    ::close(c);
    ::close(srv);
    return total == 0;
}

int send_side(const char* ip, std::uint16_t port, const char* intf) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::perror("connect");
        return 1;
    }
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    std::printf("connected to %s:%u\n", ip, port);

    ef::Vi vi;
    if (!vi.open(intf, 0, 511, EF_VI_TX_TIMESTAMPS)) return 1;
    ef::Frames frames;
    if (!frames.alloc(vi, kSlots, kSlotBytes)) return 1;

    std::uint8_t* slot = frames.at(0);
    const char message[] = "an order that went out through ef_vi";
    const int len = static_cast<int>(sizeof(message) - 1);

    onload_delegated_send ds{};
    ds.headers = slot;
    ds.headers_len = kHeaderRoom;
    const enum onload_delegated_send_rc rc = onload_delegated_send_prepare(
        fd, len, ONLOAD_DELEGATED_SEND_FLAG_RESOLVE_ARP, &ds);
    if (rc != ONLOAD_DELEGATED_SEND_RC_OK) {
        static const char* why[] = {"ok",         "not an onload tcp socket",
                                    "headers too small", "send queue busy",
                                    "send window closed", "no arp",
                                    "congestion window closed"};
        std::fprintf(stderr, "onload_delegated_send_prepare: %s (%d)\n",
                     rc < 7 ? why[rc] : "?", static_cast<int>(rc));
        if (rc == ONLOAD_DELEGATED_SEND_RC_BAD_SOCKET) {
            std::fprintf(stderr, "  run this under onload\n");
        }
        return 1;
    }
    std::printf("prepare gave %d bytes of headers, mss %d, send window %d\n",
                ds.headers_len, ds.mss, ds.send_wnd);

    std::memcpy(slot + ds.headers_len, message, static_cast<std::size_t>(len));
    onload_delegated_send_tcp_update(&ds, len, 1);

    const std::size_t frame = static_cast<std::size_t>(ds.headers_len) +
                              static_cast<std::size_t>(len);
    int trc;
    while ((trc = ef_vi_transmit(vi.get(), frames.dma(0), static_cast<int>(frame), 0)) ==
           -EAGAIN) {
    }
    if (trc < 0) {
        std::fprintf(stderr, "ef_vi_transmit: %d\n", trc);
        return 1;
    }

    iovec iov{const_cast<char*>(message), static_cast<std::size_t>(len)};
    if (onload_delegated_send_complete(fd, &iov, 1, 0) < 0) {
        std::perror("onload_delegated_send_complete");
        return 1;
    }
    std::printf("sent %zu bytes on the wire\n", frame);

    const double tps = tsc::ticks_per_ns();
    const std::uint64_t deadline = tsc::now() + static_cast<std::uint64_t>(2e9 * tps);
    ef_event evs[16];
    bool stamped = false;
    while (!stamped && tsc::now() < deadline) {
        const int n = ef_eventq_poll(vi.get(), evs, 16);
        for (int i = 0; i < n; ++i) {
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_TX_WITH_TIMESTAMP) continue;
            const std::uint64_t ns = EF_EVENT_TX_WITH_TIMESTAMP_SEC(evs[i]) * 1000000000ull +
                                     EF_EVENT_TX_WITH_TIMESTAMP_NSEC(evs[i]);
            std::printf("transmit timestamp %" PRIu64 " ns from the card\n", ns);
            stamped = true;
        }
    }
    if (!stamped) {
        std::fprintf(stderr, "no transmit timestamp came back\n");
        return 1;
    }

    ::close(fd);
    std::printf("delegated sends work on this machine\n");
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--listen") == 0) {
        return listen_side(argv[2],
                           static_cast<std::uint16_t>(std::atoi(argv[3])));
    }
    if (argc >= 5 && std::strcmp(argv[1], "--send") == 0) {
        return send_side(argv[2], static_cast<std::uint16_t>(std::atoi(argv[3])),
                         argv[4]);
    }
    std::fprintf(stderr,
                 "usage: ds_check --listen IP PORT\n"
                 "       ds_check --send IP PORT INTERFACE   (run under onload)\n");
    return 2;
}
