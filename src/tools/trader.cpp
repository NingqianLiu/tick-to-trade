// The thing being measured: it takes market data off the card, keeps an order book, and
// decides whether to send an order.
//
// This file uses ef_vi heavily. Every call has an explanation beside it, and there is a
// reference for the whole interface in ef_vi.md at the root of the repository.
//
// A few ideas make the rest read easily:
//   virtual interface   a private path to the card, asked for by this process. It comes
//   (ef_vi)             with a receive ring, a transmit ring and an event queue, all
//                       three mapped into user space, so sending and receiving never
//                       enter the kernel. A card can carry many of them at once without
//                       them interfering.
//   descriptor ring     a circle of entries, each holding the address of one buffer as
//                       the card sees it. The buffers have to be allocated, registered,
//                       posted and reclaimed by us - in socket programming the kernel
//                       does all of that.
//   event queue         where the card leaves messages: a packet arrived, a frame went
//                       out.
//   event (ef_event)    one of those messages. It is a union, and what it holds depends
//                       on its type.
//   request id          a number we give an entry. The card knows nothing about our
//   (ef_request_id)     pointers; it hands the number back and we look the address up.
//
// One thing is the opposite of sockets, and remembering it prevents most mistakes: there
// the kernel manages the buffers, the ordering and the retransmission; here the kernel is
// not on the path at all and every one of those is ours to do.
//
// There are only two threads, and only one of them does any work:
//   the main thread   ask the card for packets, update the book, check the signal, send
//                     the order - all of it
//   the ack thread    one job only: collect the TCP acknowledgements coming back
//
// There used to be a design where one core polled and several others each kept the books
// of their share of the securities. That is gone: a real desk follows the few dozen or
// few hundred names it trades, one core is enough for those, and none of the difficulties
// of splitting the work arise.
//
// Receiving, though, is all or nothing. The exchange sends one stream carrying the whole
// market and there is no way to subscribe to part of it. --symbols limits which
// securities get a book, not which ones arrive.
//
// It runs inside the network namespace that holds the receiving card:
//
//   ip netns exec trader ./build/trader --reference results/prev_close_121225.csv

// htons and htonl, which put an integer in the order the wire uses. The card's filters
// want it that way: the port and the address below both go through them before being
// handed over.
#include <arpa/inet.h>
// IPPROTO_UDP and IPPROTO_TCP. A filter has to name the protocol it wants: UDP for the
// market data, TCP for the acknowledgements.
#include <netinet/in.h>
// TCP_NODELAY and the other kernel socket options. Not one is used here - our TCP is our
// own and never goes through the kernel, so there is no socket to set anything on. It
// still compiles without this; it simply has not been cleared out.
#include <netinet/tcp.h>
// pthread_self and pthread_setaffinity_np, which pin() below uses to fix a thread to one
// core.
#include <pthread.h>
// cpu_set_t and the CPU_ZERO and CPU_SET macros that fill it in: pin() clears the set,
// lights the one bit it wants and passes it to the call above.
#include <sched.h>
// mallopt, for changing how the allocator behaves. It is called twice at start up: once
// to stop freed memory being handed back to the system, once to keep large allocations
// off mmap. Both have the same purpose - never go to the kernel for a page during a run.
#include <malloc.h>
// mlockall with MCL_CURRENT and MCL_FUTURE, which pins every page of the process in
// physical memory. It is off by default (the lock_memory option is never set) and the
// code is kept.
#include <sys/mman.h>
// mkdir, for creating the directory before the statistics are written.
#include <sys/stat.h>
// socket() and bind(). None of them is used here either - where socket appears in this
// file it is in a comment, as a comparison. Another include left over.
#include <sys/socket.h>
// iovec and writev, for writing several pieces at once. Also unused: a frame is filled by
// copying bytes into a send slot.
#include <sys/uio.h>
// close and read. Not used directly any more either - the reference prices are read with
// fopen and fgets - but other system headers want it and it costs nothing.
#include <unistd.h>

// The Onload extension interface, the onload_ functions. None is used any more: the order
// path went through Onload until it was replaced by our own TCP, which saved 810
// nanoseconds an order. This line is what that change left behind.
#include <onload/extensions.h>

// etherfabric/ef_vi.h and etherfabric/vi.h below have confusingly similar names. This one
// is what things are: the types for a virtual interface, the event queue, the descriptor
// rings and an event. Bypassing the kernel rests on it - not one system call is made to
// send or receive.
#include <etherfabric/ef_vi.h>
// The other half: what can be done with them - opening an interface, installing filters,
// asking the card for its MAC, reading hardware timestamps. What is used here is
// ef_eventq_poll (has the card left anything), ef_vi_receive_init and push (posting an
// empty buffer to the receive ring), ef_vi_transmit (handing a frame over),
// ef_filter_spec_* (saying which packets to accept),
// ef_vi_receive_get_precise_timestamp (the moment the card stamped on a packet), and
// ef_vi_start_transmit_warm and stop (a send that goes nowhere, to warm the path).
#include <etherfabric/vi.h>

// std::atomic. Only three things are shared between the two threads: a flag saying the
// run is over, how much has been drained, and one counter. Atomics rather than a lock,
// because nothing on the hot path may be able to go to sleep.
#include <atomic>
// PRIu64 and friends. The format for a 64 bit integer differs between platforms, so
// writing %lu breaks on the next machine. Thirty odd lines of statistics use these.
#include <cinttypes>
// fopen, fgets, fprintf and fclose, for reading the reference prices and the symbol list,
// and for writing the csv files and the log at the end.
#include <cstdio>
// strtoul, atoi and abort: the first two for the command line and the prices, abort for
// stopping outright when pinning a thread fails.
#include <cstdlib>
// memcpy, memset, strcmp, strchr and strerror - comparing flag names, cutting a csv line,
// copying bytes into a send slot.
#include <cstring>
// std::min and std::max, used only in the wrap up: finding the worst window and clamping
// an index.
#include <algorithm>
// std::unique_ptr and std::make_unique. The shard structure is hundreds of megabytes,
// which would overflow the stack, so it lives on the heap.
#include <memory>
// std::string, the key when reference prices are looked up by ticker, and for building
// output paths.
#include <string>
// std::thread. Exactly one extra thread is started: the one that reads acknowledgements.
#include <thread>
// The hash table used at start up to map a ticker to its previous close. Only at start
// up - it allocates, which is the last thing wanted on a hot path.
#include <unordered_map>
// The set of tickers to trade, asked once per security as the exchange names them.
#include <unordered_set>
// std::vector, which is nearly every block of memory sized by a count here: the price
// table, a row per window, and the parallel rows of raw samples.
#include <vector>

// book::Imbalance, the signal: how far apart the shares on the best three prices of each
// side are, and whether that is past the threshold.
#include "book/imbalance.hpp"
// book::OrderBook, the book itself, which holds two tables of its own - orders by id, and
// shares by price. It is the largest piece of memory here and the most heavily touched.
#include "book/order_book.hpp"
// sample::Log, a row of numbers only ever appended to.
//
// Why not just the histogram: above twenty microseconds its buckets are two microseconds
// wide, so a percentile read from it is a bucket edge. Comparing the p99.9 of two runs
// means going back to the raw samples.
#include "common/samples.hpp"
// The cfg:: constants, such as how many bytes a send slot holds. Keeping them in one
// place is what lets a static_assert catch "this frame does not fit" at compile time.
#include "common/settings.hpp"
// tsc::now, which reads the processor's counter, and tsc::ticks_per_ns. Every timing on
// the hot path uses them: a read is twenty or thirty cycles and never enters the kernel.
#include "common/tsc.hpp"
// win::Params and win::Tracker, which answer which stretch of the day a moment belongs
// to: warm up, measurement window, tail, or full speed replay. The answer comes entirely
// from the timestamp inside the message, which is why both ends can work it out
// separately and still agree.
#include "common/window.hpp"
// Cutting one UDP packet into its ITCH messages - one message or several hundred.
#include "itch/framing.hpp"
// Our thin cover over ef_vi: ef::Vi for an interface, ef::Frames for memory registered
// with the card. It keeps the boilerplate of opening an interface out of this file, and
// out of the replay's.
#include "net/ef.hpp"
// eth::ipv4, which builds an address out of four numbers, and the offsets of the fields
// in an Ethernet header.
#include "net/eth.hpp"
// Parsing MoldUDP64, the layer wrapped around the market data. It carries the sequence
// numbers that show whether anything was lost.
#include "net/mold.hpp"
// hist::Hist, the latency histogram that samples are counted into and percentiles read
// from. The p50 and p99 printed at the end come from it - and above twenty microseconds
// its buckets are two thousand nanoseconds wide, so the tail is recomputed from the raw
// samples above.
#include "common/hist.hpp"
// Huge page allocation.
//
// Why huge pages: the book is hundreds of megabytes, and in ordinary four kilobyte pages
// the address translation cache cannot hold it, so every access first walks a table in
// memory. At two megabytes a page the table is five hundred times smaller and nearly
// every access hits.
#include "common/huge.hpp"
// Reading the card's own counters. Read once before a run and once after: if they moved,
// the card dropped something and that run's samples are incomplete.
#include "net/nic_stats.hpp"
// mintcp::Conn, the small piece of TCP written here - a handshake, sending in order,
// receiving acknowledgements, retransmitting on a timeout, and nothing else. Replacing
// Onload with it saved 810 nanoseconds an order.
#include "net/mintcp.hpp"
// Packing an OUCH order. The prefill inside matters: an order is fifty bytes of which
// only five fields differ each time, so the rest is written into the send slot at start
// up and the moment a signal fires only those five are filled in.
#include "net/ouch.hpp"

// Everything here is in an anonymous namespace, so it is visible only inside this
// translation unit. More than avoiding name clashes: the compiler knows nothing outside
// can use it, which frees its hand to inline and optimise.
namespace {
struct Options {
    // Which port the market data comes in on. This is the second port of the Solarflare
    // card; the name was changed by the vendor and it changes again if the card is moved.
    const char* intf = "enp129s0f1";
    // Where the previous close csv is. The price layer uses it to cut each security a band
    // of price space.
    // The default is the file for 06-12-26, and that day's data is gone - so this has to be
    // given on every run.
    const char* reference = "results/prev_close_061226.csv";
    // Which securities we trade, one ticker per line.
    //
    // This is not "subscribe to these". The exchange sends one stream carrying the whole
    // market and there is no way to take part of it, so everything arrives either way; the
    // rest simply get no book.
    //
    // Without this file the universe is the whole market, over eleven thousand names: six
    // times the orders and a latency of ten milliseconds instead of three thousand
    // nanoseconds. Nothing reports it either - the only sign is the universe line in the log.
    const char* symbols = nullptr;
    // Which multicast address the data is sent to, needed when the filter is installed on
    // the card.
    // The range beginning with 239 is reserved for internal use and never leaves the site.
    std::uint32_t dst_ip = eth::ipv4(239, 9, 9, 1);
    // The destination port of the feed. A filter is installed on an address and a port
    // together, and both have to match before a packet is accepted.
    std::uint16_t dst_port = 26477;
    // The port of the second path. The producer sends the same data to both ports and
    // whichever arrives first is used - which is what an exchange really does, two physical
    // links backing each other up.
    // Zero means this run uses one path only.
    std::uint16_t dst_port_b = 0;
    // Which core to start pinning at. The polling thread takes it and the acknowledgement
    // thread takes the one after.
    //
    // Why pinning is not optional: without it the operating system moves a thread to
    // another core whenever it likes, and everything in cache is lost when it does. No firm
    // that really does low latency would allow that.
    //
    // It also decides which half of the machine's memory the huge pages come from - see the
    // huge::choose line further down.
    int cpu_base = 4;
    // The signal threshold, as a percent. An order goes out when the shares on the best
    // three prices of one side are that far ahead of the other.
    // Raising it sends fewer orders, lowering it more. It decides how many orders a run
    // sends altogether, so two runs being compared on latency have to use the same value.
    std::uint32_t threshold = 75;
    // Which way the book is built: true is bucketed by message type and walked in seven
    // passes, which is the default; false applies messages one at a time in the order they
    // arrived.
    // The passes do not save price level work - measured in session, only 0.1% - what they
    // buy is the chance for work that waits on memory to overlap.
    // The new path has to be the default. The only flag is --one-at-a-time going back the
    // other way, so that forgetting an argument cannot leave a whole run quietly on the old
    // path with nothing in the log to show it.
    bool group = true;
    // Within one run, alternate polls between parsing as the book is built and gathering
    // first, timing each separately.
    // It is how to measure what splitting parsing from building is worth on its own:
    // comparing across runs cannot show it, because the same code varies twenty percent
    // from run to run and the thing being measured is far smaller than that.
    bool split_ab = false;
    // How long without receiving anything counts as the end of a run, in milliseconds.
    // Quiet is the signal rather than a message from anybody: the producer finishes and
    // leaves without saying so. Two seconds is a judgement - longer than the market's
    // longest quiet stretch, shorter than a person's patience.
    std::uint64_t idle_ms = 2000;
    // Where orders go. Without it a run only receives data and works out signals, and not
    // one order is sent.
    const char* order_ip = nullptr;
    // Which port at the far end.
    std::uint16_t order_port = 45100;
    // Our own address on this connection.
    // It is needed because this TCP connection is ours rather than the kernel's: every
    // packet header has to carry it, and the card's filter uses it to recognise the
    // acknowledgements coming back.
    const char* order_local = "10.9.9.2";
    // A row per poll - the market time, how many packets, how many ITCH messages - written
    // as csv into this directory at the end. Empty means no statistics. It is a raw record
    // with nothing merged; percentiles are left to be worked out afterwards.
    const char* stat = nullptr;
    // Time the segments of each order without writing the large per poll file.
    // They are separate because that file runs to hundreds of megabytes, while looking at
    // the segments only needs events.csv.
    bool segments = false;
    // Which directory the latency samples go to. Without it nothing is written and a few
    // percentiles are printed at the end.
    // Comparing two branches means recomputing from the raw samples in this file rather
    // than using the printed numbers - above twenty microseconds those are histogram bucket
    // edges, not measured values.
    const char* out = nullptr;
    // Nothing reads this any more; it is the field left over after its flag was removed.
    bool batch_packets = false;
    // Whether the orders of one poll are gathered into a single message.
    // Always true now; the flag that turned it off is gone.
    bool coalesce = true;
    // Always false now; the flag that turned it on is gone.
    // (It pinned every page of the process in memory so none could be swapped out.)
    bool lock_memory = false;
// That is the end of Options. The constants below are not part of it; they are for the
// whole file.
};

// How many send slots there are. One slot holds one frame, and sixty four are used in turn.
// Why sixty four is enough: at the busiest an order goes out about every four hundred
// microseconds, while the card reports a frame sent within a few microseconds, so a slot
// comes free again almost at once. Sixty four is far more than needed.
constexpr std::size_t kOrderSlots = 64;
// How many bytes at the front of a slot are left for protocol headers.
constexpr int kHeaderRoom = 128;
// How many orders fit in one message.
// Twenty eight comes out of the arithmetic: an order is fifty bytes on the wire, an
// Ethernet frame holds 1500, take away twenty of IP header and twenty of TCP and 1460 is
// left, which divided by fifty is twenty eight.
constexpr std::size_t kMaxPerFrame = 28;
// The two lines below are compile time checks rather than run time ones. They guarantee a
// slot holds a whole frame, and a configuration where it does not fails to compile instead
// of writing past the end of a slot during a run.
// There are two because the two paths compute it differently: through Onload the header
// length has to be asked for, so the reserved kHeaderRoom is used; on our own connection
// the header is a fixed fifty four bytes and that is used instead.
static_assert(kHeaderRoom + kMaxPerFrame * 50 <= cfg::kOrderSlotBytes,
              "a full message of orders must fit one transmit slot");
// The second path: our own header plus a full frame of orders also has to fit one slot.
static_assert(mintcp::kHeaderLen + kMaxPerFrame * 50 <= cfg::kOrderSlotBytes,
              "our own header and a full message must fit one transmit slot");

// How many events one poll may take off the event queue.
// This number has always been the real limit rather than the card, which has over four
// thousand receive descriptors.
// Before changing it, check what else is sized by it. Going from 64 to 256 missed one
// array, a deep poll wrote past the end of it, and the trader died on the spot - with
// neither the compiler nor the unit tests catching it.
constexpr std::size_t kMaxPollEvents = 256;

// How many ITCH messages one packet can hold. A slot is 512 bytes; take away the short
// prefix the card writes in front, the Ethernet header and the MoldUDP64 header, and divide
// what is left by the shortest message there is (2 bytes of length and 12 of body).
constexpr std::size_t kMaxMsgsPerPacket =
    (cfg::kRxSlotBytes - cfg::kRxPrefixBytes - eth::kHeaderBytes - mold::kHeaderLen) / 14;
// The most one poll can gather. Changing kMaxPollEvents or the size of a slot changes this
// too, and the arrays below are all sized by it - changing one alone writes out of bounds.
constexpr std::size_t kMaxHits = kMaxPollEvents * kMaxMsgsPerPacket;

// Everything the working core owns, in one place.
//
// The name misleads. A shard sounds like one of several; there is only ever one, and the
// program creates exactly one from beginning to end. The name is left over from an earlier
// design that split the securities across several cores, one shard each. That design is
// gone, as the top of the file explains, and the name has not been changed. Read it as
// "everything the working thread has".
//
// Why so much is packed into one structure: it lives in one contiguous piece of memory on
// the heap, so these fields sit next to each other and the ones used together are likely to
// land on the same cache line. As separate globals the linker would scatter them.
struct Shard {
    // The constructor takes four things: how many orders the table must hold, how much
    // price space to claim in machine words, the percent threshold for the signal, and the
    // window parameters.
    //
    // The braces are empty and all the work is in the list after the colon - the
    // initialiser list, which says these members are built with these arguments as they
    // come into being rather than being built empty and then assigned. It has to be written
    // that way, because those book members have no default constructor: they take memory
    // the moment they exist.
    //
    // This line takes several hundred megabytes on the spot and never asks for more. Once
    // the run starts there is not one malloc on the hot path - it would be several
    // microseconds, and the tail would be nothing else.
    //
    // The last argument is 65536 x 8 spaces: eight bytes of ticker per security, filled
    // with spaces to begin with.
    Shard(std::size_t orders, std::size_t level_words, std::uint32_t pct,
          const win::Params& wp)
        : book(orders, level_words), signal(pct), phase(wp), symbols(65536 * 8, ' ') {}

    // The order book of every security. This is the largest piece of memory and the most
    // heavily accessed.
    book::OrderBook book;
    // The signal: how far apart the shares on the best three prices of each side are.
    book::Imbalance signal;
    // Which stretch of the day this moment belongs to - warm up, measurement window, tail
    // or full speed.
    // The answer comes entirely from the timestamp the message carries, so nobody has to
    // tell anybody anything: the producer and the consumer work it out separately and reach
    // the same answer.
    win::Tracker phase;
    // How many messages arrived, how many really changed the book, and how many of each
    // side.
    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    // The part of applied that fell inside a measurement window. The order rate uses it as
    // the denominator: an order only counts towards the latency inside a window too, so the
    // two agree.
    std::uint64_t applied_window = 0;
    // The three things that can go wrong with the feed, each counted.
    // gaps is a break in the sequence, meaning packets went missing; duplicates is the same
    // sequence number arriving twice; lapped is us being too slow and the card overwriting a
    // packet we had not taken yet.
    // A clean run has zero of all three; anything else means the samples are incomplete.
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0;
    // What the next packet's sequence number should be. Each packet moves it on by one, and
    // a mismatch is one of the two problems above.
    std::uint64_t expect_seq = 0;
    // Whether the first packet has arrived. What the first sequence number will be is not
    // known in advance, so this is what tells "this is the first packet" apart from "the
    // sequence broke".
    bool started = false;

    // The interface orders go out on, and a send buffer of its own. It is a second
    // interface, separate from the one receiving market data.
    ef::Vi tx;
    // The memory handed to the card, cut into sixty four slots.
    // It has to be registered memory: the card reaches into it by direct memory access, and
    // the address the card sees is not the address this process sees - registering is what
    // lines the two up.
    ef::Frames txbuf;
    // Which slot the next frame is written into. The sixty four are used in turn.
    std::size_t next_slot = 0;
    // The number given to each order, always increasing. It is how the far end says which
    // order it is reporting on.
    std::uint32_t user_ref = 1;
    // How many were sent, turned away, timestamped, reclaimed, and turned away for want of
    // a free slot.
    std::uint64_t sent = 0, refused = 0, stamped = 0, reaped = 0, no_slot = 0;
    // How many were turned away because the peer's window could not hold them, and what the
    // peer said it could still take at the moment each was turned away.
    // Counted apart from no_slot, because the other reason for turning an order away is
    // running out of send slots, and the two are fixed differently.
    std::uint64_t wnd_block = 0;
    // At those moments, the smallest and the largest number of bytes the peer said it could
    // still take.
    // The two together say what to do about it: consistently small means the peer is slow to
    // read, while swinging up and down means we sent a burst. The smallest starts at the
    // largest possible value so the first comparison takes.
    std::uint32_t wnd_min = 0xffffffffu, wnd_max = 0;
    // How many frames were handed to the card.
    // It is tempting to read this as the same thing as the count of orders above. It is not:
    // a frame holds up to twenty eight orders, so there are usually far fewer frames than
    // orders.
    // What a send slot holds is a frame rather than an order, so deciding whether a slot is
    // free has to use this number.
    std::uint64_t frames = 0;
    // The state for a send that goes nowhere, used to warm the order path.
    //
    // The problem it solves: the send path only runs when a signal fires, and in a quiet
    // stretch thousands of messages go by without one. That code and the data it uses fall
    // out of cache, and when an order finally has to go, the first one pays for fetching all
    // of it back.
    //
    // The card supports going through the whole send sequence and throwing the packet away -
    // the path runs, the caches warm, and not one byte reaches the wire. Socket programming
    // has nothing like it.
    ef_vi_tx_warm_state warm_state{};
    // How long since the last real send, counted in messages, and how many warming sends
    // there have been.
    std::uint64_t since_send = 0, warmed = 0;
    // The record of one order.
    //
    // It is not the order about to be sent. The bytes of the order were written into a send
    // slot long before; not one byte here takes part in sending - this holds how the order
    // came about.
    //
    // Why it is kept: the latency cannot be worked out at the moment of sending. All that is
    // known then is when it was handed to the card, not when it really reached the wire.
    // That has to wait for the card to report, and only then can the two be subtracted.
    // These fields are what waits for that moment.
    //
    // One per order rather than one per frame: a frame may hold several orders, and each of
    // them was triggered by a different market data message with its own arrival time.
    struct Order {
        // The time the card stamped on the packet that triggered this order. The whole
        // latency is measured from it.
        // That is the card's clock, and the fields below on the processor's clock are not
        // the same ruler.
        std::uint64_t rx = 0;
        // When the poll that took that packet returned, on the processor's clock.
        std::uint64_t poll = 0;
        // When this order entered the send path, on the processor's clock.
        std::uint64_t in = 0;
        // When each of the four steps of the send path finished: the fields filled in, the
        // TCP state updated, the doorbell pressed, and out - the moment the card says the
        // bytes really reached the wire, which is back on the card's clock.
        std::uint64_t s1 = 0, s2 = 0, s3 = 0, out = 0;
        // When fetching the packet finished. A poll returns with an address in hand while
        // the body is still in memory; this less poll is how long it took to get the body.
        std::uint64_t body = 0;
        // The moment parsing finished and building the book had not started, and the moment
        // building finished and the signal had not been checked.
        // The two cut the whole "build the book, check the signal, make the order" stretch
        // into three: parsing, building, and the signal plus making the order.
        std::uint64_t parse = 0, book = 0;
        // If this order went out during the warm up before a window, how long there was
        // until the window opened, in units of ten milliseconds.
        std::uint16_t before = 0;
        // Which security.
        std::uint16_t sym = 0;
        // How many packets that poll took, and which of them triggered this order.
        // With those two a slow sample can be read: "the sixtieth packet of sixty".
        std::uint16_t polln = 0, polli = 0;
        // Whether this sample counts. The ones from a warm up do not.
        bool counted = false;
    // That is the record of an order. It is a plain holder with no methods at all - the hot
    // path only ever writes fields into it and never calls anything on it.
    };
    // The most recently computed shares on the best three prices, for both sides of every
    // security.
    // Indexed by the security number directly, two entries each, which is why it is 1<<17
    // rather than 1<<16; every page is touched before the run so that a first write never
    // stops to ask the kernel for one.
    //
    // It looks much like prev_top below, but they are not the same thing:
    //   top_cache  what the book says at this moment, overwritten on every recount
    //   prev_top   what it said at the last signal check, for working out what changed
    // Without the second there is only "there is more now", never "it just became more".
    std::vector<std::uint64_t> top_cache = std::vector<std::uint64_t>(1u << 17, 0);
    // When fetching the packets of this poll finished; every order of this batch copies this
    // one value.
    std::uint64_t cur_body = 0;
    // The same idea, one value per batch: when parsing finished and when building the book
    // finished.
    std::uint64_t cur_parse = 0, cur_book = 0;
    // The orders written into the frame being gathered but not yet handed to the card.
    Order pend[kMaxPerFrame];
    // How many are in that frame. Twenty eight of them, or the end of this batch of
    // messages, sends them all together.
    std::size_t pend_n = 0;
    // Orders handed to the card and waiting for it to report, kept by slot, one frame each.
    Order flight[kOrderSlots][kMaxPerFrame];
    // How many orders each slot holds. When the card reports a frame sent, this is what says
    // which orders in that slot to work the latency out for.
    std::uint16_t flight_n[kOrderSlots] = {};
    // Whether several orders are gathered into one message. Always true now; the flag that
    // turned it off is gone.
    bool coalesce = true;
    // The whole state of our own TCP connection: the sequence numbers, the peer's window,
    // what the header to send looks like.
    // It replaces a socket in the kernel. It lives here rather than being global because it
    // belongs with the send slots below - a retransmission has to look at both.
    mintcp::Conn conn;
    // Which byte of a slot the orders start at, which is to say how long the protocol header
    // in front of them is.
    //
    // It looks like it should be a constant. On our own connection it is one - fifty four
    // bytes, fourteen of Ethernet, twenty of IP and twenty of TCP - but on the old path
    // through Onload the header had to be asked for and could differ each time. This field
    // is what lets both paths share the same code.
    // It starts at kHeaderRoom, 128, and is set to the real length once the handshake is
    // done.
    std::size_t hdr_len = kHeaderRoom;
    // When a slot really becomes free: when the far end has acknowledged it, not when the
    // card says it went out.
    //
    // Because TCP allows a packet to be lost and sent again, and sending it again means
    // sending the bytes of that slot exactly as they are - so until the far end acknowledges
    // them, not one byte of that slot may change.
    //
    // slot_end  where the last byte of this slot sits in the stream
    // slot_at   when this slot was sent, for deciding whether a timeout means resending it
    std::uint32_t slot_end[kOrderSlots] = {};
    // When this slot was handed to the card, on the processor's clock.
    // A timeout is now minus this, and past a threshold the slot is sent again exactly as
    // it is.
    std::uint64_t slot_at[kOrderSlots] = {};
    // How many bytes the frame in this slot has.
    //
    // Why it is stored rather than worked out as "orders in this slot times fifty": that
    // count is cleared the moment the card reports the frame sent, while the far end
    // acknowledges much later. If a retransmission were needed in between, the length would
    // come out as zero - a header with nothing behind it, which does not fill the hole and
    // leaves the connection stuck forever.
    std::uint16_t slot_len[kOrderSlots] = {};
    // How many frames the far end has acknowledged. Against the frames count above it says
    // how many are still in flight, which is how many slots cannot yet be freed.
    std::uint64_t acked_frames = 0;
    // How many times a timeout caused a retransmission. With two ports cabled together and
    // no switch in between this should be zero; anything else means something else is
    // wrong and needs looking at.
    std::uint64_t resends = 0;
    // How many times the peer said it was closing. Non zero needs looking at; normally it
    // is zero.
    std::uint64_t peer_gone = 0;
    // The arrival time belonging to each slot, kept for the subtraction when the card
    // reports.
    std::uint64_t at_slot[kOrderSlots] = {};
    // Whether the orders in this slot count - the ones from a warm up do not.
    bool counted[kOrderSlots] = {};
    // For an order sent during a warm up, how long there was until the window opened, in
    // units of ten milliseconds.
    // It exists to answer how long a warm up really has to be: plot the samples against it
    // and wherever the curve flattens is the answer. The 500 milliseconds used now is a
    // guess with nothing behind it.
    std::uint16_t before[kOrderSlots] = {};
    // Two parallel rows of raw samples for exactly that question: settle_at is how long
    // before the window this order was, settle_ns is what its latency was.
    // Drawn as one curve, wherever it flattens is how long the warm up should be.
    sample::Log settle_at, settle_ns;
    // The real latency histogram. The p50 and p99 printed at the end are read from it.
    hist::Hist latency;
    // The warm up's samples are kept here and thrown away at the end.
    // A warm up still walks the whole path, to bring the code and the data into cache - but
    // its own numbers must not reach the report, or what is measured is how slow a cold
    // start is.
    hist::Hist warmup;
    // How many messages fell inside a measurement window, and how many orders went out
    // during the stretch replayed at the real speed.
    // The two read together say whether a run has enough samples: with too few orders the
    // tail percentiles jump about.
    std::uint64_t window_messages = 0, paced_orders = 0;
    // Whether a window counts depends on the warm up in front of it really having run dry
    // once - the card's queue empty and nothing backed up in our own hands.
    // A window entered with a backlog left over from the stretch before measures that
    // backlog rather than the latency.
    win::Phase was = win::Phase::kGap;
    // A mark of how far along we were at the moment it ran dry. Compared with where we are
    // now, it says whether we have fallen behind again since.
    std::uint64_t drain_mark = 0;
    // caught_up is "this warm up really did run dry once"; window_ok is "this window
    // counts".
    // The first is the condition for the second: without running dry, the window was
    // entered with a backlog and has to be thrown away.
    bool caught_up = false, window_ok = false;
    // How many windows there were, how many were thrown away, and how many samples went
    // with them.
    // The proportion thrown away has to be reported - throw away too many and what is left
    // no longer stands for the day.
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
    // Whether this run replays the whole day at the real speed rather than measuring one
    // stretch in every thirty two.
    bool every_unit = false;
    // How many windows to let by at the start before counting. The first few minutes after
    // the open are the busiest of the day, and letting some by is what allows the two parts
    // of a morning to be compared.
    std::uint64_t skip = 0;
    // How many to count after that. Zero means all of them.
    std::uint64_t keep = 0;
    // The same samples the histogram receives, kept here as individual numbers rather than
    // gathered into buckets.
    // Keeping them raw is necessary: above twenty microseconds a bucket is two microseconds
    // wide, so a percentile read from the histogram is a bucket edge and not a real value.
    // Comparing the p99.9 of two runs means recomputing from here.
    sample::Log raw;
    // A row per window.
    //
    // Why they are kept apart: a full day has two or three thousand orders past ten
    // milliseconds, and merged into one number there is no saying when they happened -
    // spread evenly through the day, or all in the first minutes after the open. Per window
    // it can be seen.
    struct WindowStat {
        // How many samples this window has, and how many of them are past ten milliseconds.
        std::uint32_t samples = 0, over_ms = 0;
        // The slowest order of this window.
        std::uint64_t worst = 0;
    // That is one row per window.
    };
    // Sized generously: a sampled day is 681 windows, and replaying a whole day at the real
    // speed is thirty times that.
    std::vector<WindowStat> per_window = std::vector<WindowStat>(32768);
    // Which window we are in.
    std::size_t at_window = 0;
    // The arrays below are parallel: row i of each is about the same order.
    // What has to be stored about an order is more than a count of nanoseconds - a large
    // number on its own says nothing about why it is large.

    // Which window this order belongs to.
    // The tail of a whole day tends to sit in one window, so a distribution that cannot
    // tell the windows apart proves nothing.
    std::vector<std::uint16_t> raw_window;
    // The time the card stamped on the packet that triggered it.
    std::vector<std::uint64_t> raw_rx;
    // How many packets that poll took, and which of them this was.
    // They are what tells two kinds of slow apart: an order that was slow itself, and one
    // that was last in a deep poll.
    std::vector<std::uint16_t> raw_polln, raw_polli;
    // The current values of those two. Written before a packet is handed on, read when that
    // packet produces an order.
    std::uint16_t poll_n = 0, poll_i = 0;
    // A row per poll that found something: the market time of the first message of the
    // batch, how many packets it held, and how many ITCH messages altogether. Three
    // parallel arrays rather than an array of structures, which saves the padding alignment
    // would leave - under sixty million records that padding is hundreds of megabytes.
    // Allocated only when --stat names a directory; otherwise it takes not one byte.
    std::vector<std::uint64_t> stat_when;
    // How many packets this poll took. Measured, it never goes past the teens.
    std::vector<std::uint16_t> stat_pkts;
    // How many ITCH messages were in them. This is the number that really grows, up to
    // seventy - so a deep poll is not more packets but thicker ones, and a buffer has to be
    // sized by this rather than by the packets.
    std::vector<std::uint16_t> stat_msgs;
    // How far apart the arrival times of this batch were, in nanoseconds, and how long
    // since the previous poll, also in nanoseconds.
    // The two together are what answer why a packet was waiting: a span about as large as
    // the gap means we were not collecting during that time and packets piled up; a large
    // span with a small gap means the packets had arrived long before and the card or the
    // driver only let us see them now.
    std::vector<std::uint32_t> stat_span;
    // How long since the previous poll, in nanoseconds. Read together with the span above,
    // as explained there.
    std::vector<std::uint32_t> stat_gap;
    // One poll cut into three stretches, each in nanoseconds. The three moments are: the
    // packets in hand, the processing done, the order sent.
    //   proc  packets in hand to processing done. Parsing, the book, the strategy and the
    //         risk checks all count here, because they are the same kind of thing: our own
    //         C++ code.
    //   send  processing done to sent. Actually handing the order over.
    //   gap   the previous send to these packets arriving. The stretch with no work in it.
    // They are kept apart to see which stretch a stall falls in: if the stalls in each are
    // in proportion to the time that stretch takes, then the stall has nothing to do with
    // any of them and the whole core was stopped; if they gather in one stretch, that
    // stretch is the culprit.
    std::vector<std::uint32_t> stat_proc;
    // The second of those three: processing done to sent, the stretch that really hands the
    // order over.
    // The third, the stretch with no work in it, is not stored separately - stat_gap above
    // covers it.
    std::vector<std::uint32_t> stat_send;
    // Four moments as they were, not differences. Storing the moments does two things:
    // differences can be taken afterwards, in whatever way is wanted; and the processor's
    // clock and the card's clock have to be kept as pairs so that the drift between the two
    // crystals can be fitted - on this machine 460 seconds drift by 5.6 milliseconds, and
    // without taking that out, "how late we were" is nothing but drift and the real signal
    // is invisible.
    //   raw_poll  the moment the event queue returned, on the processor's clock
    //   raw_body  after the body of a packet was first read, on the processor's clock. The
    //             event only says a packet arrived; the body is in host memory, which the
    //             card cannot write into cache, so that read is most likely a real memory
    //             access - keeping it apart from the event returning is how its cost is
    //             known
    //   raw_done  processing finished, before the send, on the processor's clock
    //   raw_nic   the hardware arrival time of the first packet of the batch, on the card's
    //             clock
    std::vector<std::uint64_t> stat_raw_poll;
    // After the body of a packet was first read, on the processor's clock.
    std::vector<std::uint64_t> stat_raw_body;
    // Processing finished, before the send, on the processor's clock.
    std::vector<std::uint64_t> stat_raw_done;
    // The hardware arrival time of the first packet of the batch, on the card's clock.
    // This is the only one on the card's ruler; the three above are on the processor's. The
    // pair of them is what the drift is fitted from.
    std::vector<std::uint64_t> stat_raw_nic;
    // From the last empty poll to this one finding packets. That is the stretch where the
    // packets were there and we were not asking - the stall being looked for. The rest of
    // the idle time is the market simply being quiet.
    std::vector<std::uint32_t> stat_blind;
    // How many records have been used. Once full nothing more is written: better to record
    // less than to write out of bounds.
    std::size_t stat_used = 0;
    // How many messages this poll has counted so far, and the market time of the first.
    // Both are cleared at the start of a poll and merged into one record at the end of it.
    std::uint32_t poll_msgs = 0;
    // The market time of the first message of this poll.
    // Market time rather than local time, so the statistics line up with the day's data:
    // "the batch thirty seconds after the open" rather than "so many milliseconds into the
    // run".
    std::uint64_t poll_when = 0;
    // When the previous poll that found something was, for working out the gap to this one.
    std::uint64_t last_poll_seen = 0;
    // When the last poll that found nothing was. With it, the idle time splits in two:
    //   the last send to the last empty poll   we were asking and the market had nothing,
    //                                          which is normal
    //   the last empty poll to these packets   we were not asking while packets waited,
    //                                          and that is the stall
    // Without the split the two are mixed in one number and neither can be seen.
    std::uint64_t last_empty = 0;
    // The worst each of the three things on the empty path took, in nanoseconds, and how
    // many times each went past ten microseconds.
    // That path runs every forty nanoseconds, eleven point nine billion times in a run,
    // which cannot all be recorded - and what is being looked for is the occasional few
    // tens of microseconds, so only the worst and the count past the threshold are kept.
    std::uint64_t empty_worst[3] = {};
    // The same three, and how many times each went past ten microseconds.
    // A worst on its own cannot tell "once in a while" from "all the time"; with the count
    // it can.
    std::uint64_t empty_over[3] = {};
    // The two rows below hold, per slot, two numbers copied to every order in that slot when
    // the card reports.
    // How many packets the poll that took those orders' packets found.
    std::uint16_t at_polln[kOrderSlots] = {};
    // Which packet of that batch those orders came from.
    std::uint16_t at_polli[kOrderSlots] = {};
    // Which security the orders in each slot are for.
    // Why it is recorded: one packet producing thirteen orders is a completely different
    // thing depending on whether that is thirteen different securities or one security hit
    // thirteen times - the second is that name in a burst, the first is the market moving
    // generally.
    std::uint16_t at_sym[kOrderSlots] = {};
    // The same number moved into the raw samples: which security order i was for.
    std::vector<std::uint16_t> raw_sym;
    // The three clock reads in the life of an order: when the poll that took it returned,
    // when it entered the send path, and when it left the send path.
    // The two ends - the card receiving and the card sending - are on the card's clock;
    // these three in the middle are on the processor's.
    std::uint64_t at_poll[kOrderSlots] = {};
    // When this slot entered the send path.
    std::uint64_t at_in[kOrderSlots] = {};
    // When this slot left it, meaning the card says the bytes really reached the wire.
    std::uint64_t at_out[kOrderSlots] = {};
    // The three rows above moved into the raw samples, one entry per order.
    // Those are stored by send slot and are only valid for the short while a report is
    // awaited; these are stored by order and last until the file is written at the end.
    std::vector<std::uint64_t> raw_poll, raw_in, raw_out;
    // The four things on the send path, each timed separately.
    //
    // Why they are split: it is what decides whether gathering several orders into one
    // message is worth anything. Filling in the fields is done for every order and
    // gathering cannot save it; the other three happen once per send and can be saved. So
    // if the time goes into filling in fields, gathering is pointless.
    // Step one done: the five fields of this order are in the send slot.
    std::uint64_t at_s1[kOrderSlots] = {};
    // Step two done: the TCP header and both checksums are written and the sequence number
    // has moved on.
    std::uint64_t at_s2[kOrderSlots] = {};
    // Step three done: the doorbell has been pressed and ef_vi_transmit has returned.
    // Step four - the card saying the bytes really reached the wire - is not here; that
    // waits for the report, in at_out above.
    std::uint64_t at_s3[kOrderSlots] = {};
    std::vector<std::uint64_t> raw_s1, raw_s2, raw_s3;
    // When fetching the packet finished, one entry per order like the ones above.
    std::vector<std::uint64_t> raw_body;
    // When parsing finished and when building the book finished, also one entry per order.
    std::vector<std::uint64_t> raw_parse, raw_book;
    // Written when a poll returns, read when that poll produces an order.
    std::uint64_t poll_at = 0;
    // The signal is worked out once a packet has been handled rather than once per message.
    // The arrays below record which securities this poll touched, and what the last message
    // touching each of them looked like.
    // Only a hundred and one names are traded, so 128 entries always suffice and no overflow
    // handling is needed.
    // dirty holds the security numbers, dirty_rx the arrival time the card stamped on the
    // last message touching each, dirty_before how many tens of milliseconds before the open
    // it was if it fell in a warm up, and dirty_paced and dirty_measured whether that
    // message was in the 1:1 stretch and inside a measurement window.
    std::uint16_t dirty[128] = {};
    // The arrival time the card stamped on the first message of this batch that touched this
    // security. The latency is measured from it.
    //
    // Why the first rather than the last: it is the most conservative of the three ways of
    // counting. Using the last would leave out the time the earlier messages spent waiting
    // in the card for us - measured on the same samples, the two ways differ by 920
    // nanoseconds at p99.9 and 8,400 at p99.99. That time really was spent, so it is counted.
    //
    // It also gives something else: taking the smallest is independent of the order things
    // are processed in. Whatever a later layer does - walking by message type in several
    // passes, or running several lanes side by side - this entry still holds the same number.
    std::uint64_t dirty_rx[128] = {};
    // How many tens of milliseconds before the window that message was, if it fell in a warm
    // up.
    std::uint16_t dirty_before[128] = {};
    // Whether that message was in the stretch replayed at the real speed.
    std::uint8_t dirty_paced[128] = {};
    // Whether that message was inside a measurement window. Not the same as the one above:
    // the stretch at the real speed is longer, and a window is a short piece inside it.
    std::uint8_t dirty_measured[128] = {};
    // How many securities this batch touched. Measured, a deep poll is almost always one
    // security in a burst by itself: of the polls that took eight packets or more, 99.98%
    // involved a single name.
    std::uint32_t dirty_n = 0;
    // What the first pass parsed out goes into these six arrays, one message per index.
    // The message itself is still lying in its receive slot untouched; what is recorded here
    // is where it is and how long it is.
    // A pointer to the first byte of the body of this message.
    const std::uint8_t* hit_body[kMaxHits] = {};
    // How many bytes that body has.
    std::uint16_t hit_len[kMaxHits] = {};
    // The four things a message carries with it - the arrival time, whether it is in the 1:1
    // stretch, whether it is in a window, how long before the open - are not here. They are
    // stored per packet: see hit_pkt and the pkt_ rows below, reached through carried_of.
    // How many have been gathered in this poll so far. Cleared once the second pass is done.
    std::uint32_t hit_n = 0;

    // The ten rows below are only for the path that builds the book in seven passes, which
    // is the default. With --one-at-a-time not one byte of them is touched.
    //
    // The four buckets hold "which entry above this is", not the messages themselves - the
    // six rows above already have the pointer, the length and the arrival time, and copying
    // them again would be copying for nothing.
    // Four buckets by message type are enough rather than seven: A and F have their fields
    // in exactly the same places (F only adds four bytes of participant id at the end), and
    // E, C and X all have the order id at offset 11 and the shares at offset 19.
    std::uint32_t bucket_add[kMaxHits] = {};
    std::uint32_t bucket_repl[kMaxHits] = {};
    std::uint32_t bucket_cut[kMaxHits] = {};
    std::uint32_t bucket_del[kMaxHits] = {};
    std::uint32_t add_n = 0, repl_n = 0, cut_n = 0, del_n = 0;
    // The two rows of slot numbers for a replace. This is where the lookup that building in
    // passes saves actually is: the pass that creates the new order records its slot in
    // repl_new, the pass that finds the old one records its slot in repl_old, and the pass
    // that carries the side across reads and writes both slots directly, without a single
    // lookup.
    std::uint32_t repl_new[kMaxHits] = {};
    std::uint32_t repl_old[kMaxHits] = {};

    // Four rows of scratch space for splitting the loops.
    // The rule for splitting is: one loop only looks things up, one loop gathers the ones
    // that were found into a compact list, and only then does a loop do the work - so the
    // working loop has not a single branch in it. Measured, such a branch is worth 10.5% in
    // a loop like this, because it sits in the middle of the chain from loading an index to
    // computing an address to reaching memory, and it leaves fewer accesses in flight.
    // The results of the lookups go here first (not found is OrderTable::kNoSlot).
    std::uint32_t look[kMaxHits] = {};
    // The ones that were found, taken out of the row above and packed together.
    // The name says slot to keep it apart from the keep above, which is about whether to
    // keep a sample.
    std::uint32_t keep_slot[kMaxHits] = {};
    // In step with keep_slot: how many shares this one takes off. Only the pass that reduces
    // shares uses it.
    std::uint32_t kwant[kMaxHits] = {};
    // The slots that reached zero and really have to be removed.
    std::uint32_t zero[kMaxHits] = {};

    // Which way the book is built: false applies messages one at a time in the order they
    // arrived, true walks them in passes.
    // Every run prints a line saying which way it went - a flag that is off by default fails
    // quietly, and that kind of failure cannot be seen in the log. On 2026-08-16 a whole day
    // of comparisons was wasted on exactly that.
    bool group = true;
    // Which packet of this batch this message came in. The four things it carries with it
    // are fetched from that packet's entry.
    std::uint16_t hit_pkt[kMaxHits] = {};
    // The four rows below have one entry per packet. There are 512, twice what is needed:
    // one poll takes at most 256 events.
    // When the card received this packet.
    std::uint64_t pkt_rx[512] = {};
    // Whether this packet is in the stretch replayed at the real speed.
    std::uint8_t pkt_paced[512] = {};
    // Whether this packet is inside a measurement window that counts.
    std::uint8_t pkt_measured[512] = {};
    // If this packet fell in a warm up, how many hundredths of a second there were until the
    // window opened.
    std::uint16_t pkt_before[512] = {};
    // How many packets this batch has gathered so far. Like hit_n, it is cleared once the
    // second pass is done.
    std::uint16_t pkt_n = 0;
    // The intermediate result that splitting the work has to store: the four fields of each
    // add.
    // An entry is 12 bytes - four of shares, four of price, one of side, one of padding and
    // two of security number.
    book::OrderTable::Order add_o[kMaxHits] = {};
    // The five below exist to answer one question: splitting parsing from building the book
    // into two passes costs an extra write and an extra read, and what is that worth.
    // The way to find out is to alternate the two paths from poll to poll within one run and
    // time each. Two polls are microseconds apart, so the clock speed, the cache and the
    // other processes are the same for both and cancel out in the subtraction.
    // Running two separate rounds and comparing them does not work: the same code varies
    // twenty percent from round to round, and what is being measured is far smaller.
    // The switch. Without --split-ab it stays false and none of these fields is touched.
    bool split_ab = false;
    // Which way this poll goes. True is gather into the arrays first and build the book
    // afterwards - two passes, which is what is done now; false is build the book as soon as
    // a message is parsed, in one pass, which is what main has always done.
    // True by default, so without the switch the behaviour is exactly as before.
    bool collect = true;
    // How many polls have been done. Its parity picks the path above, so each gets half.
    std::uint64_t polls_done = 0;
    // The account for each path: how many times it was timed, how many clock ticks in total,
    // and how many messages really changed the book.
    // Index 0 is building as parsing goes, 1 is gathering first.
    std::uint64_t ab_polls[2] = {}, ab_ticks[2] = {}, ab_msgs[2] = {};
    // What the best three prices of each side held the last time a signal was worked out for
    // this security, against top_cache above.
    // The top 32 bits are the buy side and the low 32 the sell side.
    // Both in one 64 bit number so that each security is read once and written once; kept
    // apart, the same piece of work would touch two cache lines.
    // All zero means no signal has been worked out yet, and no order goes out that time -
    // without a previous value there is no saying whether it rose or fell.
    // Indexed by stock locate directly; with a hundred and one names only that many entries
    // are ever used.
    std::vector<std::uint64_t> prev_top = std::vector<std::uint64_t>(1u << 16, 0);
    // How many messages each poll took, counted without exception.
    //
    // This number is the backlog itself rather than an estimate of it: a poll can only take
    // what the card has already written and we have not yet collected.
    //
    // One entry per count rather than the latency histogram: that one has five nanosecond
    // buckets and would put "took one" and "took four" in the same place, which is exactly
    // what has to be kept apart here.
    // There are only 65 entries, 0 to 64, and anything past 64 goes in the last one - and
    // since a poll may take up to kMaxPollEvents = 256, that last entry really is used, so
    // it must not be read as "exactly 64".
    std::uint64_t depth[65] = {};
    // The eight byte ticker of each security, cut out before the open.
    // Sending an order is then copying eight bytes rather than parsing a name again.
    // The initial value is spaces rather than zeros, because fixed length text fields in the
    // order protocol are padded with spaces (ouch::prefill also lays a whole order out in
    // spaces).
    // What really reads this initial value is a security number that never appeared in the
    // directory - no order should go out for one, but if one did, spaces are at least a
    // legal ticker where zero bytes are not.
    std::vector<std::uint8_t> symbols;
    // One byte per security number: whether we trade this one.
    // Filled in before the open from the directory the exchange sends.
    //
    // An empty array means the whole market, and note that this is the other way round: it
    // does not mean none of them, it means no restriction was given.
    // The test reads size() rather than a separate flag, because size() is already in cache
    // and the test then costs one comparison.
    std::vector<std::uint8_t> traded;
    // How many securities really got price space and how many did not.
    // Running the whole market, a few dozen missing would disappear into the noise; with a
    // hundred and one names, one missing is a percent of the orders and has to be counted.
    std::uint64_t bound = 0, unbound = 0;
// That is everything the working core owns.
// It runs to several hundred megabytes, so it is created on the heap with make_unique in
// main and cannot live on the stack.
};

// Reads the reference price csv: one line per security, two columns, ticker and previous
// close.
//
// What the reference price is for: the price layer uses it to cut each security a band of
// price space, eight times up and eight times down. A security with no reference price gets
// no band and can therefore never produce a signal.
// So changing which day is replayed means changing the data file and this csv together.
// Changing only one reports nothing; it just moves where the strategy fires.
//
// Two things come back: out, which looks a price up by ticker for binding securities at the
// open, and prices, the same prices in a row, which the price layer uses to work out how
// much space to claim altogether.
bool read_reference(const char* path, std::unordered_map<std::string, std::uint32_t>* out,
                    std::vector<std::uint32_t>* prices) {
    // Opened for reading. This happens before a run and not on the hot path, so ordinary
    // file reading is fine.
    std::FILE* f = std::fopen(path, "r");
    // The caller prints the path and exits - all of this is before a run starts.
    if (f == nullptr) return false;
    // One line is a ticker and a price, twenty odd characters at most, so 128 is ample.
    char line[128];
    // Read the header away first; it is not data.
    // Failing to read it means the file is empty, which is also a failure.
    if (std::fgets(line, sizeof(line), f) == nullptr) {
        // Closed before leaving. It is about to exit anyway, but leaving a file open is a
        // bad habit.
        std::fclose(f);
        return false;
    }
    // One security per line, to the end of the file.
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        // The file has two columns, so the first comma is the divider.
        // A proper csv parser might seem called for here. It is not: a proper parser has to
        // handle commas inside quotes and fields spanning lines, while this file is
        // generated by us and a ticker cannot contain a comma. Another layer of parsing
        // would only be another place to get it wrong.
        char* comma = std::strchr(line, ',');
        // No comma means a blank line or a broken one. It is skipped rather than treated as
        // an error - there is usually a blank line at the end of a file.
        if (comma == nullptr) continue;
        // Turning the comma into a string terminator leaves line holding the ticker alone,
        // with nothing copied out.
        *comma = '\0';
        // After the comma is the price, in the same units as ITCH - hundredths of a cent as
        // a whole number - so nothing is converted here.
        const auto price = static_cast<std::uint32_t>(std::strtoul(comma + 1, nullptr, 10));
        // Kept by ticker, which is how a price is found when the feed names a security at
        // the open.
        (*out)[line] = price;
        // And kept in order, which the price layer uses to size the whole block.
        prices->push_back(price);
    }
    std::fclose(f);
    // Both outputs are filled in; the caller uses them to bind securities and to size the
    // price layer.
    return true;
}

// Failing to pin and saying nothing is worse than not pinning at all: the thread ends up on
// whatever core the operating system allows, while the log still names a core it never ran
// on - and then nobody knows what conditions that run's numbers were measured under.
//
// This is not hypothetical: it happened the first time cores were reserved. The shell that
// started the program was itself restricted to other cores, so every attempt to pin
// returned EINVAL.
void pin(int cpu) {
    // A negative number means this thread is not to be pinned, which the diagnostic tools
    // use.
    if (cpu < 0) return;
    // A bitmap with one bit per core - ninety six of them on this machine.
    // It is created on the stack, so it holds whatever the last call left there; see the
    // next line.
    cpu_set_t set;
    // It has to be cleared first. Without that the thread would be allowed to run on a
    // random collection of cores.
    CPU_ZERO(&set);
    // Only the one bit, so the thread can only run on that core.
    CPU_SET(cpu, &set);
    // Pins this thread rather than the process, so every thread calls it once after
    // starting.
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    // This function does not set errno; it returns the error code, so rc has to be caught
    // and checked.
    if (rc != 0) {
        // The code turned into words, along with how to fix it - the usual cause is that
        // the shell which started the program was not put in the group that owns those
        // cores.
        std::fprintf(stderr,
                     "cannot run on cpu %d: %s\n"
                     "reserved cores need the process inside the group that owns "
                     "them: scripts/isolate_cores.sh run -- ...\n",
                     cpu, std::strerror(rc));
        // Stop, rather than print and carry on. Carrying on scatters the run's threads over
        // other cores while the report names the ones we meant to use.
        std::abort();
    }
}

// Turns "10.9.9.1" from the command line into an integer.
// Only called at start up, so sscanf is fine.
bool parse_ip(const char* s, std::uint32_t* out) {
    // One per part. unsigned rather than uint8_t, because sscanf's %u wants something as
    // wide as an int; giving it a one byte variable writes past it.
    unsigned a, b, c, d;
    // All four or nothing. Fewer means a typo, and the caller reports it and exits.
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    // eth::ipv4 assembles the integer. That function is constexpr, but the values here are
    // known only at run time, so it runs as an ordinary function.
    *out = eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                     static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d));
    // The result is in out.
    // Note that each part is not checked against 256 - 999.1.1.1 is truncated rather than
    // reported. The command line is ours, so nothing here defends against it.
    return true;
}


// Opens an interface used only for sending orders.
// It is entirely separate from the one receiving market data: not a byte is sent on that
// one, and not a byte is received on this one - the acknowledgements coming back use a
// third, handled by the acknowledgement thread.
bool open_order_path(Shard* self, const char* intf) {
    // This TCP connection is ours, so no socket is opened in the kernel at all.
    //
    // Why opening one anyway would break things: the far end accepts a single connection.
    // A kernel socket here would take it, leaving ours waiting in the queue - the handshake
    // would still be answered, the data would never be read, and nothing would report
    // anything.
    // An interface that only sends.
    // The receive ring gets 0 entries: nothing is received on it, and acknowledgements come
    // in on a queue of their own.
    // The transmit ring gets 511.
    // Two flags: hardware timestamps on the frames going out, which the last segment of the
    // timing needs, and always push - pressing the doorbell carries the descriptor and the
    // frame across with it, which saves the card a trip back into host memory, measured at
    // about sixty nanoseconds.
    if (!self->tx.open(intf, 0, 511,
                       static_cast<enum ef_vi_flags>(EF_VI_TX_TIMESTAMPS |
                                                     EF_VI_TX_PUSH_ALWAYS))) {
        // The two usual reasons are a mistyped port name and the card having no interfaces
        // left - a card only carries so many.
        // The caller prints a line and exits; a run that cannot send orders is pointless.
        return false;
    }
    // Sixty four send slots of 2048 bytes. A frame gathers at most twenty eight orders,
    // fourteen hundred bytes.
    if (!self->txbuf.alloc(self->tx, kOrderSlots, cfg::kOrderSlotBytes)) return false;
    // The interface and the buffer are ready, but orders still cannot go out: the handshake
    // has to happen first, and only once the header length is known can seed_slots write the
    // constant bytes into the slots.
    return true;
}

// Writes the bytes of an order that never change into every position an order could land
// in.
//
// This is one of the reasons the send path is fast.
// An order is fifty bytes, of which only five fields differ each time: the order number,
// the side, the shares, the ticker and the price. Everything else is written once and never
// touched again - when a signal fires, only those five have to be written.
//
// Why it is not done at start up and has to wait until after the first prepare: until then
// the header length is unknown, and so is where the orders actually land. Written in the
// wrong place, every order would go out with three zero bytes in it, and the far end would
// quietly refuse it.
void seed_slots(Shard* self) {
    // All sixty four slots.
    // Not just the current one: frames use the sixty four in turn, and a slot left out would
    // send a pile of zero bytes when its turn came.
    for (std::size_t i = 0; i < kOrderSlots; ++i) {
        // The orders sit behind the header, so the start skips it.
        // hdr_len is asked for on the Onload path rather than being a constant.
        std::uint8_t* base = self->txbuf.at(i) + self->hdr_len;
        // Up to twenty eight orders in a slot, and every one of their positions gets the
        // constant bytes.
        // That is 64 x 28 = 1,792 of them, done once at start up.
        for (std::size_t k = 0; k < kMaxPerFrame; ++k) {
            // The bytes of an order that never change: the message type, the flags that are
            // fixed, and the parts that are the same in every order. When a signal fires,
            // five fields are all that is left to write.
            ouch::prefill(base + k * ouch::kOrderPacketLen);
        }
    }
}

// Declared here because it is defined below, and both flush_orders and send_order need it
// first.
void reap(Shard* self);

// Hands the orders gathered into this frame to the card, all at once.
//
// The name flush suggests it sees the bytes onto the wire. It does not: it goes as far as
// handing them to the card, and when it returns the packet is most likely still queued
// inside it. When the bytes really reach the wire arrives later as an event saying the frame
// was sent - see reap.
//
// The three things done here happen once per frame rather than once per order: writing one
// protocol header, pressing the doorbell once, recording one send slot.
// That is the whole point of gathering several orders into one frame - twenty eight orders
// share one fixed cost.
void flush_orders(Shard* self) {
    // Nothing gathered, nothing to do.
    // The end of the polling loop calls this unconditionally, and the vast majority of polls
    // send no order at all, so this line is the most travelled path in the function.
    if (self->pend_n == 0) return;
    // The count is copied first: pend_n is cleared below and the number is still needed.
    const std::size_t n = self->pend_n;
    // An order is a fixed fifty bytes, so the data length of this frame is a multiplication.
    const std::size_t bytes = n * ouch::kOrderPacketLen;
    // Which slot this frame is in. send_order has already written the bytes into it one
    // order at a time; this only works out where it starts.
    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    // The orders start after the header, which is where the constant bytes seed_slots wrote
    // are.
    std::uint8_t* msg = slot + self->hdr_len;
    // The first point of the segment timing: building the frame begins.
    const std::uint64_t f1 = tsc::now();
    // How long the frame turns out to be, headers included, worked out by the line below.
    std::size_t frame = 0;
    // Three more moments, all filled in inside the braces below.
    // f2 is point three of the segment timing: the moment the TCP header and both checksums
    // are written.
    // They are declared here rather than where they are used because they have to outlive
    // that block - after it they are copied to every order.
    std::uint64_t f2 = 0, f3 = 0, f4 = 0;
    // The slot number is kept: next_slot moves on below and the number is needed at the end.
    const std::size_t id = self->next_slot;
    // Everything inside this pair of braces is handing one frame over.
    // They are there for historical reasons: this used to be an if and an else, our own TCP
    // and Onload, and when the Onload path went the braces stayed.
    {
        // With a socket, this line is send().
        // What the kernel does behind send() is all here now:
        //   in front of the orders already laid out, write fourteen bytes of Ethernet
        //   header, twenty of IP and twenty of TCP - fifty four altogether, which is
        //   hdr_len;
        //   compute the IP and the TCP checksums;
        //   move the sequence number, how far this stream has sent, on by bytes.
        // What comes back is the length of the whole frame, the fifty four plus the orders.
        //
        // There is one thing send() does that this does not: copy. send() copies the bytes
        // into a kernel buffer, after which the caller's own buffer can be changed; here
        // there is no copy - this slot is the only copy there is, and it stays until the far
        // end says it arrived.
        frame = self->conn.send(slot, msg, bytes, mintcp::kAck | mintcp::kPsh);
        // The second point of the segment timing: the header and the checksums are written
        // and the card has not been touched.
        f2 = tsc::now();
        // This line is the doorbell: it tells the card that this slot holds frame bytes and
        // to send them.
        //
        // The four arguments:
        //   the first    which interface. self->tx is the one for orders, entirely separate
        //                from the one receiving market data.
        //   the second   the address of this slot as the card sees it, not a pointer in this
        //                process - the card uses direct memory access and has addresses of
        //                its own. The correspondence was established when that memory was
        //                registered in alloc, and dma() looks it up.
        //   the third    how many bytes this frame is. The whole frame, Ethernet header, IP
        //                header and TCP header included, not the bytes of the orders.
        //   the fourth   the number we gave this slot. The card knows nothing about our
        //                pointers; it puts the number into the event queue unchanged, and we
        //                look up which slot it was.
        //
        // When this returns, the bytes are not on the wire - only the descriptor is in the
        // transmit ring. When they really go out arrives as an event saying the frame was
        // sent (see reap), which is also why the latency cannot be worked out at this
        // moment.
        //
        // A negative return means the transmit ring was full and it was not accepted.
        if (ef_vi_transmit(self->tx.get(), self->txbuf.dma(id),
                           static_cast<int>(frame),
                           static_cast<ef_request_id>(id)) < 0) {
            // Every order in this frame counts as turned away.
            // It should not happen in a normal run: the transmit ring has 511 entries and at
            // most sixty four are ever used.
            self->refused += n;
            // The gathered orders are dropped and not retried - the market has moved on and
            // sending a stale order is pointless.
            self->pend_n = 0;
            // Note that next_slot has not moved, so the next frame goes in this same slot.
            return;
        }
        // Handed over, so the orders in this frame count towards the total sent.
        self->sent += n;
        // The third point of the segment timing: the doorbell has been pressed.
        f3 = tsc::now();
        // f4 belongs to the moment the bytes really reach the wire, which is not known here
        // - it waits for the card to report. f3 stands in for now, and reap overwrites it
        // with the real value.
        f4 = f3;
        // Where the last byte of this slot sits in the stream.
        // Only once the far end's acknowledgement passes that number is the slot really
        // free.
        self->slot_end[id] = self->conn.snd_nxt();
        // When this slot was handed over, which a timeout subtracts from the current time.
        self->slot_at[id] = f3;
        // How many bytes the frame in this slot has. It has to be stored rather than worked
        // out later as orders times fifty: that count is cleared the moment the card
        // reports, while the far end acknowledges much later.
        self->slot_len[id] = static_cast<std::uint16_t>(frame);
    }
    // The moments of each step are copied to every order in the frame - those steps really
    // were done together for all of them.
    // Only filling in the fields differs per order, and that one each order recorded for
    // itself as it was written.
    for (std::size_t k = 0; k < n; ++k) {
        // A reference, so it is changed in place rather than copied.
        Shard::Order& o = self->pend[k];
        // The three moments that happen once per frame, the same for every order in it.
        o.s2 = f2; o.s3 = f3; o.out = f4;
        // s1 is "this order's fields are written", which differs per order and was recorded
        // in send_order. There is one case where it is zero: the experimental path that
        // skips building the book. There the moment the frame was built stands in.
        if (o.s1 == 0) o.s1 = f1;
        // The record moves from being gathered to waiting for the card to report.
        self->flight[id][k] = o;
    }
    // How many orders this slot holds, which reap needs in order to know how many to work
    // the latency out for.
    self->flight_n[id] = static_cast<std::uint16_t>(n);
    // The arrival time belonging to this slot, taken from the first order.
    // It doubles as a flag: non zero means the slot is occupied, until reap reclaims it.
    self->at_slot[id] = self->pend[0].rx;
    // The count goes back to zero and the next order starts gathering at the first position
    // of a frame again.
    // The bytes are not cleared - the next write covers them, and the length of a frame is
    // worked out from the count, so anything not written is never sent.
    self->pend_n = 0;
    // One more frame handed over. Against acked_frames it says how many are still in flight.
    ++self->frames;
    // On to the next slot. The modulo is what makes it go round the sixty four, and by the
    // time it comes back round that slot has long been free.
    self->next_slot = (self->next_slot + 1) % kOrderSlots;
    // How many messages since the last real send goes back to zero. It is what decides
    // whether to do a warming send.
    self->since_send = 0;
}

// Makes one order.
//
// Despite the name, this usually sends nothing: it writes the bytes of one order into the
// frame being gathered. That frame is only handed to the card by flush_orders above, once
// twenty eight orders have gathered or this poll has finished.
// (Only in the case where it fills up does the last line here call flush_orders itself.)
//
// The price comes from the other side of the book: deciding the buyers are about to win
// means taking the best offer.
//
// When fixed is not zero the price is taken from it rather than asked of the book. Only the
// experimental path that skips building the book uses that: with no book there is nothing to
// ask. The normal path passes 0 and behaves as before.
void send_order(Shard* self, std::uint16_t sym, char side, std::uint64_t rx_ts,
                bool keep, std::uint16_t before, std::uint32_t fixed = 0) {
    // Two clock reads per order rather than two per message.
    //
    // Why that is affordable: there are a twentieth as many orders as packets, and an order
    // takes over a thousand nanoseconds to walk this path while two clock reads are a few
    // tens - the observation does not change what is being observed, so it stays in for real
    // runs.
    //
    // And it is the only way to separate two things: how much of an order's time went into
    // this path, and how much of it was simply waiting for us to come and collect it.
    const std::uint64_t enter = tsc::now();
    // When a slot is free: not when the card says the frame went out, but when the far end
    // says the bytes arrived. Until then it may have to be sent again, and this slot is the
    // only copy.
    {
        // First reclaim the slots the far end has acknowledged.
        // The test is whether the number of the last byte of a slot has been passed by the
        // far end's acknowledgement.
        // before_eq rather than a plain comparison: sequence numbers are 32 bits and wrap
        // through zero, and a plain comparison answers backwards at the wrap.
        while (self->acked_frames < self->frames &&
               mintcp::before_eq(self->slot_end[self->acked_frames % kOrderSlots],
                                 self->conn.snd_una())) {
            // That slot is free, so move on and look at the next.
            ++self->acked_frames;
        }
        // How much the peer says it can still take. Sending more than that makes it drop
        // what will not fit, and the only sign is that it then waits forever for the hole to
        // be filled.
        // This check is not optional: sixty four slots of twenty eight orders is ninety
        // kilobytes, while without window scaling the peer can say at most sixty five.
        // Only the room this one order really needs is counted. It used to count a whole
        // frame of twenty eight, because the check came before the gathering and how many
        // would gather was not yet known; but the ones already gathered have not been handed
        // to the card and their bytes are not in in_flight either, so "what is gathered plus
        // this one" is the accurate number.
        if (self->conn.in_flight() +
                (self->pend_n + 1) * ouch::kOrderPacketLen >
            self->conn.peer_wnd()) {
            // How many bytes the peer says it can take at this moment.
            const std::uint32_t w = self->conn.peer_wnd();
            // The smallest and the largest window reported at a moment like this.
            // The two are what say whether the peer is always tight or only occasionally.
            if (w < self->wnd_min) self->wnd_min = w;
            // And the largest.
            if (w > self->wnd_max) self->wnd_max = w;
            // Orders turned away by the window, counted apart from the total turned away.
            // The other reason for turning one away is running out of send slots, and the
            // two are fixed differently.
            ++self->wnd_block;
            // It also counts towards the total turned away.
            // One order is counted once by each of these, so the two do not add up to the
            // total and must not be added.
            ++self->no_slot;
            // This order does not go out. The signal really did fire; we simply could not
            // send it.
            return;
        }
        // Stop sending when the send slots are nearly gone.
        //
        // The test is "fewer than four left" rather than "none left". Those four are for the
        // handshake and for retransmissions, which need slots too - waiting until zero means
        // not even a retransmission can go out, and a retransmission is the only thing that
        // can bring the connection back.
        //
        // And to say it again: a slot is free when the peer has acknowledged it, not when
        // the card says it went out - before that it is the only copy of that frame and may
        // be needed again at any moment.
        if (self->frames - self->acked_frames >= kOrderSlots - 4) {
            // Printed only the first time. Once this happens it usually happens thousands of
            // times in a row, and printing each would flood the log - and the flooding itself
            // is slow enough to affect the run.
            if (self->no_slot == 0) {
                // The whole state at once: frames handed over, acknowledged and reclaimed,
                // the three sequence numbers and the peer's window. There is only one chance
                // to print it, so it is printed in full.
                std::fprintf(stderr,
                             "own tcp out of slots: frames %" PRIu64 " acked %" PRIu64
                             " reaped %" PRIu64 " una %u nxt %u wnd %u\n",
                             self->frames, self->acked_frames, self->reaped,
                             self->conn.snd_una(), self->conn.snd_nxt(),
                             self->conn.peer_wnd());
            }
            // One more turned away. The if above uses this to know whether it is the first.
            ++self->no_slot;
            // This order does not go out. The signal really did fire; we could not send it.
            return;
        }
    // On the Onload path, nothing can be sent before the header has been obtained.
    }
    // What price this order goes at, and how many shares are on the other side.
    // shares only catches the second output of best() and nobody uses it afterwards - one
    // share is always sent.
    std::uint32_t price = 0, shares = 0;
    // A taking order trades at the other side's price: to buy, ask the sell side; to sell,
    // ask the buy side.
    // Asking the wrong side gives a price that can never trade, while the program looks
    // perfectly healthy.
    const std::uint8_t take_from =
        side == ouch::kBuy ? book::PriceLevels::kSell : book::PriceLevels::kBuy;
    // The experimental path brings its own price and skips the question; the normal path
    // sends nothing when there is no answer, exactly as before.
    if (fixed != 0) {
        // The experimental path: the price came from the caller and the book is not asked.
        price = fixed;
    // The normal path: ask the book for the best price on the other side.
    } else if (!self->book.best(sym, take_from, &price, &shares)) {
        // No answer, meaning that side has no orders at all, so this one does not go out.
        // That is normal rather than an error - before the open many securities have orders
        // on one side only.
        return;
    }

    // Which slot this order is written into.
    // It is the slot of the frame being gathered, not the next one: next_slot only moves on
    // once this frame has really been handed to the card.
    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    // Where the first order of the frame goes: past the header.
    // hdr_len is not a constant. On our own connection it is fifty four; on the Onload path
    // it has to be asked for - so it has to be read here rather than written in.
    std::uint8_t* base = slot + self->hdr_len;
    // Where this one goes: past however many have gathered already.
    std::uint8_t* msg = base + self->pend_n * ouch::kOrderPacketLen;
    // This line is the whole of writing bytes on the send path: five fields.
    // With fewer than twenty eight gathered, the positions after it are simply not counted
    // in the length of the frame and do not have to be cleared.
    // One share always - a choice of the test rig, so that how many orders were sent and how
    // many shares traded do not interfere with each other.
    // The ticker was cut out before the open and arranged by security number, so this is one
    // copy of eight bytes.
    ouch::fill(msg, self->user_ref, side, 1, &self->symbols[sym * 8], price);
    // The client order id: our own number, which the exchange repeats back in its reports.
    ouch::set_cl_ord_id(msg, self->user_ref);
    // On to the next number for the next order.
    // It serves as two things at once: the order number OUCH keeps, and the client order id.
    // The same number is written in both places, so a report matches on either.
    ++self->user_ref;
    // Everything above is work no order can avoid - its fields have to be written somewhere.
    // Whatever else changes, that part cannot be saved.
    // Take a place in the frame being gathered and raise the count.
    // A reference, so it is filled in place rather than built and copied.
    Shard::Order& o = self->pend[self->pend_n++];
    // The time the card stamped on the packet that triggered this order, which is where the
    // latency starts.
    o.rx = rx_ts;
    // When the poll that took that packet returned.
    o.poll = self->poll_at;
    // When the bodies of that batch of packets came to hand.
    o.body = self->cur_body;
    // When that batch finished parsing, and when the book finished changing.
    o.parse = self->cur_parse;
    o.book = self->cur_book;
    // When this order entered the send path, which is the clock read at the top of this
    // function.
    o.in = enter;
    // The fields are written. This is the only moment that differs per order - the three
    // after it belong to the whole frame and flush_orders copies them together.
    o.s1 = tsc::now();
    // If it fell in a warm up, how many tens of milliseconds there were until the window
    // opened.
    o.before = before;
    // Which security. It is what tells "one packet hit thirteen securities" apart from "one
    // security was hit thirteen times".
    o.sym = sym;
    // How many packets that poll took.
    o.polln = self->poll_n;
    // Which of them this order came from.
    o.polli = self->poll_i;
    // Whether this sample goes into the real report. The warm up's do not, and the caller
    // decides.
    o.counted = keep;
    // Twenty eight gathered means send. Any left over go out when the main loop has finished
    // with this batch of packets.
    // (The first half of the test is for turning gathering off, and the flag that did that
    // is gone, so it can no longer be reached.)
    if (!self->coalesce || self->pend_n == kMaxPerFrame) flush_orders(self);
}

// Reads the clock with a fence on each side. rdtsc is not a barrier and out of order
// execution can move it; over a few microseconds that does not matter, but fetching a packet
// may be only one or two hundred nanoseconds, where twenty or thirty cycles of movement is
// ten percent. lfence means nothing after it may start until every read before it has
// finished.
// Not __rdtscp: it only holds the front, an lfence is still needed behind it, and it also
// returns a core number - and with the thread pinned to one core that number is of no use.
[[gnu::always_inline]] inline std::uint64_t fenced_now() noexcept {
    // The fence in front stops the clock being read before the reads ahead of it have
    // finished.
    __builtin_ia32_lfence();
    // The read itself, which is twenty or thirty cycles; the two fences are what cost.
    const std::uint64_t t = tsc::now();
    // The fence behind stops the reads after it being brought in front of the clock read.
    // Both are needed; one alone holds only one direction.
    __builtin_ia32_lfence();
    return t;
}

// Finds the card address of the machine at the other end.
// It reads the table the operating system has already built rather than asking on the wire:
// this is done once in the life of the program and is not worth a second implementation.
//
// After a reboot this is the step that trips people up: that table is empty again, so this
// fails - and the error reported is "cannot open our own connection", which gives no hint
// that the address table is the problem.
// A single ping to the far end before a run is enough.
bool read_neighbour(std::uint32_t peer_ip, std::uint8_t out[6]) {
    // /proc/net/arp is the kernel's table of neighbours it has seen, laid out as text.
    // One line per neighbour: the address, the hardware type, the flags, the card address,
    // the mask and which port it is on.
    std::FILE* f = std::fopen("/proc/net/arp", "r");
    // It should always open, unless this runs somewhere without /proc mounted.
    if (f == nullptr) return false;
    // A line of this file is a few dozen characters, so 256 is ample.
    char line[256];
    // The first line is the header rather than data, so it is read away.
    // Failing to read it means the file is empty, which is also a failure.
    if (std::fgets(line, sizeof(line), f) == nullptr) { std::fclose(f); return false; }
    // Whether it was found. The flag is needed because the file still has to be closed after
    // breaking out, so the loop cannot simply return.
    bool found = false;
    // One neighbour per line, from the top.
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        // The four parts of the address and the six bytes of the card address.
        unsigned a, b, c, d, m[6];
        // The flags column, which is a string of hex.
        char flags[16];
        // Cutting the line up. The %*s in the middle reads a field and throws it away,
        // skipping the hardware type.
        // %15s reads at most fifteen characters, leaving room for the terminator so nothing
        // is written past the end of flags.
        // Eleven things have to come out - four parts of the address, the flags and six
        // bytes - and fewer means the line is not in the expected shape.
        if (std::sscanf(line, "%u.%u.%u.%u %*s %15s %x:%x:%x:%x:%x:%x", &a, &b, &c,
                        &d, flags, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 11) {
            // A line that will not come apart is skipped rather than treated as an error.
            continue;
        }
        // The four parts of this line assembled into an integer and compared with the one
        // being looked for.
        if (eth::ipv4(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                      static_cast<std::uint8_t>(c),
                      static_cast<std::uint8_t>(d)) != peer_ip) {
            // Not the machine wanted, so on to the next line.
            continue;
        }
        // Some lines are lookups that never completed: the address is all zeros and the
        // flags say so. They are skipped.
        if (std::strtol(flags, nullptr, 16) == 0) continue;
        // The six bytes go to the caller. sscanf's %x produces unsigned values, so each is
        // narrowed to a byte.
        for (int i = 0; i < 6; ++i) out[i] = static_cast<std::uint8_t>(m[i]);
        // Recorded so that the code after the loop knows whether to answer true or false.
        found = true;
        // One address has one line, so there is no reason to read further.
        break;
    }
    // The file is closed here rather than inside the loop, because the loop breaks out.
    std::fclose(f);
    // A false makes the caller exit: without the far end's card address the Ethernet header
    // we build ourselves cannot be filled in, and not one byte can be sent.
    return found;
}

// Handles one acknowledgement coming back from the exchange.
//
// It might look as though the exchange's reports - whether an order traded and so on -
// would be handled here. They are not: on this connection we only send, and the only useful
// things coming back are the acknowledgement number and the window. The number says how much
// of what was sent arrived; the window says how much more can be sent. Those two are what
// say which send slot can be freed and whether sending may continue.
// So there is not even any reordering to do here - there is no stream of data coming back.
// This runs on the acknowledgement thread rather than the one sending orders.
// Why they are apart: together, the acknowledgement that would free the send window ends up
// behind thousands of market data packets.
// Apart, the orders turned away in the last ten minutes of a session went from 9,485 to 0.
// Three arguments: the state of this connection, where the packet received starts, and how
// many bytes it has.
void take_ack(Shard* self, const std::uint8_t* p, std::size_t len) {
    // Too short even for a header, so it is a bad packet. Dropped, and not an error.
    if (len < mintcp::kHeaderLen) return;
    // Once the peer says it is closing, every order sent after that falls into a hole.
    // Nothing here can repair it, but it has to be counted - a non zero at the end means that
    // run's percentiles are worthless.
    // Without the count, such a run would look exactly like a clean one.
    // The flags byte of the TCP header; either of two bits means the peer is closing:
    // RST is "I do not recognise this connection", FIN is "I have finished sending and want
    // to close".
    if ((p[mintcp::kTcpFlagsOff] & (mintcp::kRst | mintcp::kFin)) != 0) {
        // Counted. A non zero at the end means that run's percentiles are worthless.
        ++self->peer_gone;
    }
    // In a packet without the ACK flag, the acknowledgement number means nothing.
    if ((p[mintcp::kTcpFlagsOff] & mintcp::kAck) == 0) return;
    // Both are needed: the number moves "how much they have received" along, and the window
    // decides how much more may be sent.
    // The return value is deliberately ignored - it says whether this one moved anything
    // along, while reclaiming send slots is worked out afresh by resend_stale below and does
    // not depend on it.
    (void)self->conn.on_ack(mintcp::get32(p + mintcp::kTcpAckOff),
                            mintcp::get16(p + mintcp::kTcpWinOff));
}

// Sends again a frame that has gone too long without being acknowledged.
//
// With a socket this function never has to be written - the kernel's stack retransmits by
// itself. With the kernel taken out, it falls to us.
//
// Why it cannot be skipped: TCP delivers in order. A frame lost in the middle leaves a hole
// at the far end, and until the hole is filled, not one byte of what arrives after it can be
// delivered - so losing a frame is not "one order missing", it is "no order ever arrives
// again".
//
// Owning the connection means owning the retransmission; there is no stack to do it. This is
// the other side of the 810 nanoseconds that writing our own TCP bought.
void resend_stale(Shard* self, std::uint64_t rto) {
    // First cross off the frames that have been acknowledged.
    // The test is whether the sequence number of the last byte of a frame is at or before
    // where the peer has acknowledged to.
    // before_eq rather than <=, because the numbers are 32 bits and wrap.
    while (self->acked_frames < self->frames &&
           mintcp::before_eq(self->slot_end[self->acked_frames % kOrderSlots],
                             self->conn.snd_una())) {
        // Acknowledged, so move on one.
        // This is the same logic as the piece in send_order, and both are needed: that one
        // reclaims while sending, this one reclaims when there is nothing to send.
        ++self->acked_frames;
    }
    // Everything is acknowledged and there is nothing to send again, which is the path taken
    // nearly all the time.
    if (self->acked_frames == self->frames) return;
    // Something is still waiting, and the oldest of them is the most likely to be the one
    // lost.
    // That is the frame after the acknowledged ones, the oldest still waiting.
    // The modulo is because acked_frames only ever increases while there are sixty four
    // slots, which it goes round.
    const std::size_t id = self->acked_frames % kOrderSlots;
    // Not yet past the timeout, so wait longer. rto is worked out from the round trip time
    // by the caller and passed in.
    if (tsc::now() - self->slot_at[id] < rto) return;
    // Only the oldest frame is resent at a time. If more than one was lost, the next round
    // takes the next one.
    // The slot goes out again exactly as it is.
    // Its bytes were never touched, so nothing has to be rebuilt - which is what the rule
    // "a slot may not be reused until it is acknowledged" buys.
    // ef_vi_transmit is the equivalent of send(), with two important differences.
    // The four arguments: the interface, the address of this frame as the card sees it, how
    // many bytes it is, and the number we gave it.
    //
    // The two differences from send():
    //   send() copies the bytes into a kernel buffer, after which the caller's own buffer
    //   can be changed. Here there is no copy - the card reads this memory directly, so not
    //   one byte may move until it has finished.
    //   send() blocks or returns EAGAIN when its buffer is full. Here a full ring simply
    //   returns a negative number and nothing is waited for.
    //
    // It hands over rather than sends: when this returns, the packet is most likely not on
    // the wire yet. The moment it really goes out arrives as a completion event from the
    // card (see reap).
    // That number comes back unchanged inside the completion, which is how the frame that
    // finished is recognised.
    //
    // And once handed over, not one byte of that memory may move until the card has read it.
    // That is where the rule "a slot may not be reused until it is acknowledged" comes from.
    (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(id),
                         static_cast<int>(self->slot_len[id]),
                         static_cast<ef_request_id>(id));
    // The clock restarts, so the next round does not treat it as timed out and send it
    // again.
    self->slot_at[id] = tsc::now();
    // A non zero here needs looking at: this link is a direct cable and should not lose
    // packets.
    ++self->resends;
}

// Walking through the TCP handshake ourselves: find the far end's card address, send a SYN,
// wait for the SYN-ACK, and answer with an ACK.
//
// With a socket this whole function is one call to connect(). Every step the kernel takes
// behind that call has to be written out below.
//
// Why it is written out: there is no kernel on this path, and so no kernel socket to do the
// handshake. The TCP state machine - sequence numbers, acknowledgements, retransmission,
// windows - is all in src/net/mintcp.hpp, written here.
//
// It is worth the trouble: replacing the Onload path saved 810 nanoseconds an order and took
// the send function from 1,540 nanoseconds to 110. It is the single largest gain in this
// project.
//
// It runs once before trading starts, borrowing the acknowledgement queue to receive the
// replies - by then the card already has the filter that recognises this connection.
bool shake_hands(Shard* self, ef::Vi& rx, ef::Frames& rxbuf, std::size_t prefix,
                 std::size_t rx_slots, std::size_t* next_post,
                 std::uint32_t our_ip, std::uint16_t our_port,
                 std::uint32_t peer_ip, std::uint16_t peer_port,
                 const char* intf) {
    // The port name is no longer needed - the far end's address is read from the system's
    // table rather than asked for on the wire. The argument stays so that callers do not
    // have to change.
    (void)intf;
    // ef_vi_get_mac asks the driver for the card address of the port this interface is on.
    // Three arguments: the interface, the driver handle, and six bytes to fill in.
    // Asked for rather than written into a configuration file, so changing the card or the
    // port changes nothing.
    std::uint8_t our_mac[6];
    // Without it the Ethernet header cannot be built, so the connection is given up.
    // The caller prints a line and exits - a run that cannot send orders is pointless.
    if (ef_vi_get_mac(self->tx.get(), self->tx.dh(), our_mac) < 0) return false;
    // The far end's card address.
    // This is the step that most often trips people up after a reboot.
    // The system's address table is empty again, so this line returns false.
    // And the error reported is "shard 0 could not open its own connection", which gives no
    // hint that the address table is the problem.
    // A single ping to the far end before a run is enough.
    std::uint8_t peer_mac[6];
    // Looked up in the system's table. Not found means giving up, for the reason above.
    if (!read_neighbour(peer_ip, peer_mac)) return false;

    // Both ends assembled and handed to the connection, which builds the frame template.
    eth::Endpoint us{}, them{};
    // Our card address, six bytes.
    std::memcpy(us.mac, our_mac, 6);
    // Our address. It goes into the IP header of every packet and it decides which replies
    // the card's filter accepts.
    us.ip = our_ip;
    // Our port.
    us.port = our_port;
    // The far end's address. It decides where packets go and which replies the filter takes
    // as belonging to this connection.
    them.ip = peer_ip;
    // The MAC in this structure is left empty: peer_mac above is passed to open separately
    // and does not go through here.
    them.port = peer_port;
    // The starting sequence number is the low 32 bits of the processor's counter, so that a
    // packet left over in the network from a previous run cannot be taken as part of this
    // one.
    // The 65535 at the end is the receive window we advertise. Only acknowledgements come
    // back on this connection, so it can never be used up.
    self->conn.open(us, them, peer_mac, static_cast<std::uint32_t>(tsc::now()),
                    65535);
    // On our own connection the header is a fixed fifty four bytes, rather than being asked
    // for as on the Onload path.
    self->hdr_len = mintcp::kHeaderLen;

    // The handshake frame goes into an ordinary send slot and follows the same path as any
    // order.
    std::uint8_t* slot = self->txbuf.at(self->next_slot);
    // The first packet of the handshake carries the window scale option, so that the peer
    // can state its own shift - without offering it, the most the peer could ever tell us is
    // 65,535 bytes.
    // Our own side gives 0: almost nothing comes back on this connection, so there is
    // nothing to inflate.
    std::size_t n = self->conn.send_syn(slot, 0);
    // The SYN is handed to the card. The four arguments are the same as when sending an
    // order: the interface, the address of this slot as the card sees it, how many bytes the
    // frame is, and the number we gave the slot.
    if (ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                       static_cast<int>(n),
                       static_cast<ef_request_id>(self->next_slot)) < 0) {
        // Not accepted. At start up the transmit ring is empty, so reaching here means the
        // interface was never opened properly.
        return false;
    }
    // On to the next slot. The handshake frame occupies one like any order.
    self->next_slot = (self->next_slot + 1) % kOrderSlots;

    // Wait at most three seconds for the reply.
    // A limit is essential: if the far end is not running, without one this spins forever
    // while all that can be seen from outside is a run that started and does nothing.
    // How many ticks the clock of this machine takes per nanosecond, which the next line uses
    // to turn three seconds into a number of ticks.
    const double tps = tsc::ticks_per_ns();
    // The moment three seconds from now. 3e9 is three thousand million nanoseconds.
    const std::uint64_t stop = tsc::now() + static_cast<std::uint64_t>(3e9 * tps);
    // Keep asking until the reply arrives or the three seconds are up.
    while (tsc::now() < stop) {
        // At most eight events at a time. During the handshake there is almost nothing else
        // on the wire, so eight is ample.
        ef_event evs[8];
        // Note that this polls the market data queue rather than the acknowledgement one:
        // the handshake happens before the run starts, and that queue does not exist yet.
        // ef_eventq_poll is "go and see whether the card has left anything".
        // Three arguments: which interface, an array to put the events in, and how many at
        // most.
        // It returns how many were really taken; zero means the queue was empty and nothing
        // has happened.
        // It does not block and does not enter the kernel - it reads a piece of memory we
        // can see ourselves.
        const int got = ef_eventq_poll(rx.get(), evs, 8);
        // One at a time.
        for (int i = 0; i < got; ++i) {
            // An event is a union and what it holds depends on its type. Only "a packet
            // arrived" matters here; the others, such as a frame having been sent or an
            // error, are of no interest during the handshake.
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
            // The number we gave this buffer when it was posted comes back. The card knows
            // nothing about our pointers; it returns the number unchanged and we look up
            // which buffer it was.
            const std::size_t id = EF_EVENT_RX_RQ_ID(evs[i]);
            // How many bytes this packet is, less the prefix - the card writes a short piece
            // of its own metadata in front of a packet, the hardware timestamp among it, and
            // that is not part of the packet.
            const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - prefix;
            // Where the packet really begins: the start of that buffer, past the metadata.
            const std::uint8_t* p = rxbuf.at(id) + prefix;
            // Whether this is the SYN-ACK being waited for.
            bool done = false;
            // Three things have to hold: it is long enough to hold a TCP header, the SYN bit
            // is set, and the ACK bit is set.
            // Both bits matter - a SYN on its own is the far end opening a connection to us,
            // which is not what is wanted.
            if (len >= mintcp::kHeaderLen &&
                (p[mintcp::kTcpFlagsOff] & mintcp::kSyn) != 0 &&
                (p[mintcp::kTcpFlagsOff] & mintcp::kAck) != 0) {
                // The far end's starting sequence number plus one is the first byte we
                // expect, the plus one being because the SYN itself takes a sequence number.
                self->conn.set_rcv_nxt(mintcp::get32(p + mintcp::kTcpSeqOff) + 1);
                // The peer's acknowledgement number and window are taken as well.
                // The return value is deliberately ignored - this is initialising state, not
                // asking whether anything moved along.
                (void)self->conn.on_ack(mintcp::get32(p + mintcp::kTcpAckOff),
                                        mintcp::get16(p + mintcp::kTcpWinOff));
                // The reply may carry the peer's own window scale.
                // The order matters: scaling only applies to packets after the handshake, so
                // this packet is dealt with as it is first and the shift is recorded
                // afterwards.
                self->conn.set_peer_shift(mintcp::Conn::shift_in(p, len));
                // The second step of the handshake has arrived; what follows is the final
                // ACK.
                done = true;
            }
            // The three lines below give an empty buffer back to the card.
            // This step does not exist in socket programming at all: a buffer that has been
            // used has to be posted again, and without that the receive ring soon runs dry
            // and not one packet is ever received again.
            // It happens whether or not this packet was of any use, which is why the three
            // lines are outside the if.
            //
            // Which entry to post. next_post only increases and the modulo takes it round
            // the ring.
            const std::size_t give = *next_post % rx_slots;
            // On to the next one.
            ++*next_post;
            // ef_vi_receive_init writes the address of that buffer as the card sees it, and
            // the number we gave it, into the next entry of the receive ring. It only writes
            // into the ring; the card cannot see it yet.
            (void)ef_vi_receive_init(rx.get(), rxbuf.dma(give), give);
            // ef_vi_receive_push is what tells the card there are new entries and it may
            // write packets into them.
            // The two are separate so that several entries can be posted and the doorbell
            // pressed once, sharing that cost; during the handshake one is posted at a time,
            // so they are called together.
            ef_vi_receive_push(rx.get());
            // The SYN-ACK has arrived, so the final ACK goes back and the handshake is done.
            if (done) {
                // Another slot for that ACK.
                slot = self->txbuf.at(self->next_slot);
                // A bare acknowledgement with no data at all: a null pointer and a length of
                // zero.
                n = self->conn.send(slot, nullptr, 0, mintcp::kAck);
                // Handed to the card. The return value is not checked - during the handshake
                // the transmit ring is certainly empty, and there would be nothing to do
                // about a failure here anyway.
                (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                                     static_cast<int>(n),
                                     static_cast<ef_request_id>(self->next_slot));
                // On to the next slot. The handshake has now used two of them, one for the
                // SYN and one for the ACK.
                self->next_slot = (self->next_slot + 1) % kOrderSlots;
                // Those two slots hold no orders, so nobody is waiting on them and the
                // accounting can start from a clean state.
                self->conn.on_ack(self->conn.snd_nxt(), self->conn.peer_wnd());
                // The handshake sent two frames, and the card will report them like any
                // other. Both have to be counted here, or "sent" minus "reclaimed" goes
                // negative - and being unsigned it wraps to an enormous number, after which
                // every order is turned away for want of a free slot.
                self->frames += 2;
                // And counted as acknowledged. Those two slots hold no orders and never need
                // resending, so counting them as done keeps resend_stale from watching them
                // forever.
                self->acked_frames += 2;
                // The bytes of an order that never change have to be written into every send
                // slot here - nothing else on this path writes them, and without it every
                // order would go out empty.
                //
                // This has to happen after the handshake and not before. The first handshake
                // packet carries four bytes of options, and the options sit right behind the
                // fifty four byte header - exactly where the first four bytes of an order
                // are. Done earlier, those four constant bytes would be overwritten by the
                // options, and since sending an order only writes the fields that change,
                // what was overwritten would never come back. Every time that slot came
                // round, one order would start with rubbish.
                seed_slots(self);
                // The handshake succeeded, and from here the connection can send orders: the
                // sequence numbers line up, the peer's window is known, and the constant
                // bytes are in all sixty four slots.
                return true;
            }
        }
    }
    // Three seconds went by without a SYN-ACK.
    // The usual causes are that the far end is not running, or that the filter is wrong and
    // the reply never reaches us.
    // The caller prints a line on the false and exits.
    return false;
}

// Runs the send path without sending anything, to warm it.
//
// It is tempting to think warming means sending a real order first. It does not work: a real
// order would reach the far end, which would act on it, and it would occupy a send slot of
// its own.
// ef_vi supports going through the whole send sequence and throwing the packet away. Socket
// programming has nothing like it.
//
// Why it is needed: the send path only runs when a signal fires. In a quiet stretch
// thousands of messages go by without one, and that code and the state it uses fall out of
// cache.
// When an order finally has to go, the first one pays for fetching all of it back.
//
// The card supports going through the whole sequence and throwing the packet away. The same
// path runs, the caches warm, and not one byte reaches the wire.
void warm(Shard* self) {
    // This line puts the interface into warming mode.
    // Between here and the stop below, every ef_vi_transmit walks the whole path as usual:
    // working out the address, writing the descriptor, pressing the doorbell.
    // The card then throws what it receives away instead of sending it.
    //
    // Three arguments:
    //   the interface
    //   a small piece of state, whose storage we provide, filled in by start and used by stop
    //   where the warming content comes from. Null means the one in the transmit below.
    ef_vi_start_transmit_warm(self->tx.get(), &self->warm_state, nullptr);
    // One real send call. The length of one order is enough - what is wanted is the path,
    // not the content.
    // The request id is the special EF_REQUEST_ID_MASK rather than a real slot number.
    // A warming send produces a completion too, and with a real slot number reap would treat
    // it as an actual order and work out a latency for it.
    (void)ef_vi_transmit(self->tx.get(), self->txbuf.dma(self->next_slot),
                         static_cast<int>(kHeaderRoom + ouch::kOrderPacketLen),
                         EF_REQUEST_ID_MASK);
    // Out of warming mode. The next ef_vi_transmit really goes on the wire.
    // Forgetting this line makes the card quietly throw every order away afterwards - the
    // program looks perfectly healthy and the far end receives nothing.
    ef_vi_stop_transmit_warm(self->tx.get(), &self->warm_state);
    // Counted, and printed at the end, to confirm this really is happening.
    ++self->warmed;
}

// This is the other end of the whole measurement.
//
// The name reap suggests tidying up, collecting the slots that have been used. Collecting
// them is incidental - the real job is working out the latency.
// The latency of an order cannot be worked out when it is sent, because at that moment when
// it reaches the wire is unknown; this function is the only place that knows the answer.
//
// The card reports when each frame really reached the wire.
// That moment less the arrival time of the packet that triggered the order is one wire to
// wire sample.
// Both ends are hardware timestamps the card wrote itself, with no clock read by the
// processor in between - so the number is not affected by our own code.
void reap(Shard* self) {
    // At most sixteen events at a time - the reason is in the line below.
    ef_event evs[16];
    // Go and see whether the card has left anything on the send queue.
    // Sixteen at a time: this queue only carries completions for the frames we sent
    // ourselves and never goes deep, while the market data queue takes 256 at a time.
    const int n = ef_eventq_poll(self->tx.get(), evs, 16);
    // One at a time. Taking none is by far the most common outcome - this is called on every
    // poll, and in the vast majority of them not one order was sent.
    for (int i = 0; i < n; ++i) {
        // Only send completions that carry a timestamp.
        // The warming sends above produce events too, and this line filters them out.
        if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_TX_WITH_TIMESTAMP) continue;
        // Which frame this event is about, which is to say which slot it was sent from.
        // The modulo matters: the number handed over when sending only increases, while
        // there are sixty four slots.
        const std::size_t id = EF_EVENT_TX_WITH_TIMESTAMP_RQ_ID(evs[i]) % kOrderSlots;
        // The moment this frame really reached the wire, stamped by the card.
        // It is reported as seconds and nanoseconds, so two macros read the two parts and
        // they are put together.
        // This is the card's own clock, which is not the processor's counter - but it is the
        // same clock the arrival timestamps use, so subtracting one from the other means
        // something.
        // That is also why the wire to wire number is not affected by our code.
        const std::uint64_t out =
            EF_EVENT_TX_WITH_TIMESTAMP_SEC(evs[i]) * 1000000000ull +
            EF_EVENT_TX_WITH_TIMESTAMP_NSEC(evs[i]);
        // One message has one completion, so every order inside it shares the same moment of
        // going out while each has its own arrival time.
        // Their latencies therefore differ, and they should: the one at the front was ready
        // earliest and so waited longest.
        for (std::size_t k = 0; k < self->flight_n[id]; ++k) {
            // The record of order k in this slot - the one flush_orders stored.
            const Shard::Order& o = self->flight[id][k];
            // Which packet brought this order about, and when that packet reached the card.
            const std::uint64_t in = o.rx;
            // With no arrival time, or a difference that comes out negative, the sample
            // cannot be used.
            // Arrival and departure are on the same clock of the same card, so an inversion
            // can only mean something else went wrong.
            if (in == 0 || out <= in) continue;
            // Samples from a window go into the real distribution and those from a warm up
            // into another.
            // They are kept apart because a warm up's samples must not appear in the report.
            (o.counted ? self->latency : self->warmup).add(out - in);
            // Orders from a warm up that recorded how long there was until the window are
            // also stored separately.
            // The two rows are a pair, and they exist to answer how long a warm up should be:
            // drawn as a curve, wherever it flattens is the answer.
            if (!o.counted && o.before != 0) {
                // The horizontal axis: how many tens of milliseconds before the window.
                self->settle_at.add(o.before);
                // The vertical axis: the latency at that moment.
                self->settle_ns.add(out - in);
            }
            // Everything below is only for the orders that count, meaning the ones inside a
            // measurement window that counts.
            if (o.counted) {
                // The raw number, every one of them.
                // The histogram above has two microsecond buckets past twenty microseconds,
                // so what it reports up there is a bucket edge; comparing the p99.9 of two
                // runs means recomputing from this row.
                self->raw.add(out - in);
                // These rows are sized once at start up and nothing more is written when
                // they fill - better to record a few less than to ask for memory on the hot
                // path, which is several microseconds.
                if (self->raw_window.size() < self->raw_window.capacity()) {
                    // Which window it fell in.
                    // The tail of a whole day often gathers in one window, and without the
                    // window nothing can be said about it.
                    self->raw_window.push_back(static_cast<std::uint16_t>(self->at_window));
                    // The arrays below are in step with raw_window: row i of each is about
                    // the same order, so together they can be written out as one table.
                    self->raw_rx.push_back(in);
                    // How many packets the poll that took it found.
                    self->raw_polln.push_back(o.polln);
                    // Which of that batch it came from.
                    // Read with the number above, it tells "slow in itself" apart from "last
                    // in a deep poll".
                    self->raw_polli.push_back(o.polli);
                    // Which security, which is how a burst of one security is told apart from
                    // the whole market moving.
                    self->raw_sym.push_back(o.sym);
                    // The six below are the moments this order passed through, stored as
                    // moments rather than differences - a difference can be taken afterwards,
                    // in whatever way is wanted.
                    //
                    // When the poll returned, on the processor's clock.
                    self->raw_poll.push_back(o.poll);
                    // When it entered the send path.
                    self->raw_in.push_back(o.in);
                    // When it left the send path.
                    self->raw_out.push_back(o.out);
                    // When the five fields of this order were written.
                    self->raw_s1.push_back(o.s1);
                    // When the frame's TCP header and both checksums were written.
                    self->raw_s2.push_back(o.s2);
                    // The packet bodies in hand. In time this comes before the ones above;
                    // the order here is the order the code was written in, not the order
                    // things happened.
                    self->raw_body.push_back(o.body);
                    // When the packets of that batch had all been parsed.
                    self->raw_parse.push_back(o.parse);
                    // When the book had finished changing for that batch.
                    self->raw_book.push_back(o.book);
                    // When the doorbell had been pressed and ef_vi_transmit returned.
                    self->raw_s3.push_back(o.s3);
                }
                // And a note in the table that keeps a row per window.
                // Why per window: a whole day has two or three thousand orders past ten
                // milliseconds, and as one number there is no saying whether they are spread
                // through the day or gathered in the first minutes after the open.
                if (self->at_window < self->per_window.size()) {
                    // That window's row, changed in place.
                    Shard::WindowStat& w = self->per_window[self->at_window];
                    // One more sample in this window.
                    ++w.samples;
                    // Anything past a million nanoseconds, which is a millisecond, is counted
                    // separately.
                    // A millisecond is hundreds of times the normal value, so such a sample
                    // means something else happened at that moment.
                    if (out - in > 1000000) ++w.over_ms;
                    // And the slowest of this window.
                    if (out - in > w.worst) w.worst = out - in;
                }
            }
            // This order got its hardware timestamp.
            // Compared with how many orders were sent, it says whether any went without a
            // latency being worked out.
            ++self->stamped;
        }
        // Every order in this slot has been dealt with, so the count goes to zero.
        self->flight_n[id] = 0;
        // And the arrival time is cleared. It doubles as the flag saying the slot is
        // occupied, and without clearing it the check at the end for slots not reclaimed
        // would never reach zero.
        self->at_slot[id] = 0;
        // One more frame reclaimed. Compared with the frames handed over at the end, a large
        // difference means some never came back.
        ++self->reaped;
    }
}

// Once a batch of messages has been walked, the signal is worked out once for each security
// the batch touched.
//
// The dirty in the name does not mean broken; it means this batch changed it - after a batch
// of messages, usually only one or two securities have had their book changed, and the list
// of them is called the dirty list.
// settle is settling up: turning the changes this batch gathered into signals in one go.
// It takes the shard and whether this run really sends orders; afterwards the dirty list is
// cleared and the next batch starts from nothing. There is no return value, because any
// order has already gone to the send path.
void settle_dirty(Shard* self, bool trading) {
    // This loop is the body of the function: one decision per security, in the order they
    // were touched.
    for (std::uint32_t i = 0; i < self->dirty_n; ++i) {
        // Which security this entry is about; everything below reads its book as the batch
        // left it.
        const std::uint16_t sym = self->dirty[i];
        // The shares on the best three prices of the buy side, from the price layer.
        const std::uint64_t bid3 = self->book.top3(sym, book::PriceLevels::kBuy);
        // And of the sell side; only the two together are enough to decide.
        const std::uint64_t ask3 = self->book.top3(sym, book::PriceLevels::kSell);
        // The decision itself, which comes back as one of three: buy, sell, or nothing.
        const auto what = self->signal.check(bid3, ask3);
        // What the best three held the last time this security was decided, for comparison.
        const std::uint64_t was = self->prev_top[sym];
        // Whether or not an order goes out, this time's numbers are recorded for the next
        // comparison.
        self->prev_top[sym] = (bid3 << 32) | (ask3 & 0xffffffffull);
        // Nothing to send, so on to the next security; the moment and the stretch recorded
        // in this entry are no longer needed.
        if (what == book::Imbalance::Signal::kNone) continue;
        // Being past the threshold is not enough on its own: while a security sits above it
        // for a long time, every poll would send another order even though the book has not
        // moved any further that way in between. With this test only a move further than
        // last time sends, which turns a level into an edge.
        // The previous two sides, the buy side in the top 32 bits and the sell side in the
        // low 32.
        const std::uint64_t wb = was >> 32, ws = was & 0xffffffffull;
        // Both zero means this security has never been decided; with no previous value there
        // is no direction, so nothing is sent.
        if (wb == 0 && ws == 0) continue;
        // What is compared is whether the buy side's share has grown: b1/(b1+s1) >
        // b0/(b0+s0).
        // Both sides multiplied by the denominators to keep it as multiplication, which
        // avoids a division on the hot path and introduces no floating point error.
        const bool up = bid3 * (wb + ws) > wb * (bid3 + ask3);
        // A buy signal requires the buy side's share to have really risen, a sell signal that
        // it has really fallen.
        // Exactly level sends neither, which is the same convention as the strictly greater
        // in the threshold.
        if (what == book::Imbalance::Signal::kBuy) {
            // A buy signal where the buy share did not rise is not a new step, so nothing is
            // sent.
            if (!up) continue;
        // What is left can only be a sell signal - "send nothing" was filtered out above.
        } else {
            // The same arithmetic the other way: the buy share really fell.
            // It cannot be written as !up - when they are level both up and down are false
            // and neither is sent.
            const bool down = bid3 * (wb + ws) < wb * (bid3 + ask3);
            // No fall means no new step, so nothing is sent.
            if (!down) continue;
        }
        // The signal turned into the character the order protocol uses, B or S.
        const char side =
            what == book::Imbalance::Signal::kBuy ? ouch::kBuy : ouch::kSell;
        // Counted per side, and printed at the end to see whether it only ever leans one way.
        if (side == ouch::kBuy) ++self->buys; else ++self->sells;
        // Outside the 1:1 stretch nothing is sent: the full speed stretch compresses the
        // market dozens of times, and sending there is sending far too fast.
        if (self->dirty_paced[i] == 0) continue;
        // Orders that really go out in the 1:1 stretch, which matches the number printed at
        // the end.
        ++self->paced_orders;
        // The starting point is the earliest message of this batch that touched this
        // security, which is the most conservative of the choices.
        if (trading) send_order(self, sym, side, self->dirty_rx[i],
                                self->dirty_measured[i] != 0, self->dirty_before[i]);
    }
    // The list is cleared and the next batch starts again. Zeroing the count is enough; the
    // old values in the entries are never read.
    self->dirty_n = 0;
}

// The four things a message carries with it: when the packet holding it was received,
// whether it is in the 1:1 stretch, whether it is in a measurement window, and how long
// there is until the open.
struct Carried {
    // When the card received the packet holding this message, which is where the latency
    // starts.
    std::uint64_t rx;
    // Whether it is in the stretch replayed at the real speed, which decides whether an
    // order goes out.
    std::uint8_t paced;
    // Whether it is inside a measurement window that counts, which decides whether the
    // latency counts.
    std::uint8_t measured;
    // If it fell in a warm up, how many hundredths of a second there were until the window
    // opened.
    std::uint16_t before;
};

// Fetches those four things for message i, where i is its position in this batch.
//
// All four describe the situation of the packet holding the message rather than the message
// itself, so one copy per packet is enough and each message only records which packet it
// belongs to.
// That saves ten bytes per message and takes the arrays being touched from six down to
// three.
//
// Strictly they are not identical: a packet may straddle the boundary of a measurement
// window. But a packet spans about 300 nanoseconds of market time while a window is a whole
// second, so at most one packet is misplaced at each boundary.
[[nodiscard]] inline Carried carried_of(const Shard* self, std::uint32_t i) {
    // Look up which packet this message is in and read that packet's entry.
    // Those four rows are only 512 entries, at most 4 KB, so they stay in the first level
    // cache and this extra read costs nothing.
    const std::uint16_t k = self->hit_pkt[i];
    return {self->pkt_rx[k], self->pkt_paced[k], self->pkt_measured[k],
            self->pkt_before[k]};
}

// The bookkeeping done once a message has really changed the book.
// It is a function because both paths do the same bookkeeping: one calls it as it parses,
// the other when it comes back to build the book.
// The arguments are what the message carries: which security it changed, when the card
// received the packet holding it, and which stretch of the timeline it fell in. The last says
// whether this run really sends orders.
void note_applied(Shard* self, std::uint16_t touched, std::uint64_t rx,
                  std::uint8_t paced, std::uint8_t measured, std::uint16_t before,
                  bool trading) {
    ++self->applied;
    // Only the ones inside a measurement window are counted here. A denominator added up
    // from polls.csv is five percent out either way, because shallow polls are recorded by
    // sampling one in a thousand and twenty four; this counts every one, so the order rate
    // is exact.
    if (measured != 0) ++self->applied_window;
    // Long enough since a real send that the path has most likely gone cold, so a warming
    // send goes through it.
    // At the busiest, hundreds of messages go by per order, so this only fires when things
    // are quiet.
    if (trading && ++self->since_send >= 256) {
        warm(self);
        self->since_send = 0;
    }
    // The best three are not asked for here and no signal is worked out. Messages the
    // exchange produced in the same instant belong to one event - one taking order sweeping
    // three levels sends three executions in a row - and the states in between never existed
    // as a book anything could trade against, so deciding there is deciding on a book that
    // never was. Instead the whole batch is walked and each security decided once.
    // This search is linear because a poll is nearly always about one security, so it usually
    // compares once.
    std::uint32_t d = 0;
    while (d < self->dirty_n && self->dirty[d] != touched) ++d;
    // Whether this entry is the first for this security in the batch, or the security is
    // already on the list.
    // The two have to be told apart: a fresh entry still holds the previous batch's values,
    // and all three fields have to be set rather than accumulated.
    const bool fresh = d == self->dirty_n;
    if (fresh && self->dirty_n < 128) {
        self->dirty[self->dirty_n++] = touched;
    }
    if (d < 128) {
        // The starting point is the earliest message of this batch to touch this security -
        // see the note on dirty_rx in Shard.
        if (fresh) {
            // The first time this batch touches this security, so this message is the
            // earliest.
            // It has to be written explicitly: the entry still holds the previous batch's
            // value, and a comparison would take the wrong one.
            self->dirty_rx[d] = rx;
        } else if (rx < self->dirty_rx[d]) {
            // Already touched, so only something earlier replaces it. Taking the smallest is
            // independent of the order things are processed in.
            self->dirty_rx[d] = rx;
        }
        self->dirty_paced[d] = paced;
        self->dirty_measured[d] = measured;
        self->dirty_before[d] = before;
    }
}

// The first of the two second pass paths: apply the messages to the book one at a time, in
// the order they arrived. This is the default.
void apply_stream(Shard* self, bool trading) {
    for (std::uint32_t i = 0; i < self->hit_n; ++i) {
        const itch::Message m{self->hit_body[i], self->hit_len[i]};
        std::uint16_t touched = 0;
        if (self->book.apply(m, &touched)) {
            const Carried c = carried_of(self, i);
            note_applied(self, touched, c.rx, c.paced, c.measured, c.before, trading);
        }
    }
}

// The second path: bucket the messages by type and walk them in seven passes.
//
// Why it is worth splitting: A and F are thirty six percent of the messages and D another
// thirty four, and their lookups do not depend on one another at all - done together, a
// dozen memory accesses can be in flight at once, while one at a time each waits for the
// one before it.
// It saves no price level work at all - measured in session, 0.1% - and the whole gain comes
// from whether those accesses can overlap.
//
// The order of the passes has to satisfy "create before use", and every creation comes
// before every deletion - so within a batch no slot is ever returned and taken by somebody
// else, and a slot number written down stays valid.
//
// There is one difference in meaning against applying one at a time: an E before a U or D in
// the stream ends up after them. The book comes out the same - a U or D takes the whole old
// order away, and how much it takes does not depend on an E in between - but that E finds no
// target and is skipped, so applied comes out smaller on this path. It cannot be used as a
// gate; use orders alive instead.
void apply_grouped(Shard* self, bool trading) {
    // Pass 1 - the adds. It is split into three loops: one only inserts into the order
    // table, one only changes the price levels, one only does the bookkeeping.
    //
    // Why split: inserting into the order table and changing a price level touch two
    // completely unrelated pieces of memory, and both are cold - touched for the first time,
    // not in cache. In one loop the price level access has to queue behind the order table
    // access, one order waiting for one order. Split apart, dozens of orders in the same loop
    // have accesses that do not depend on each other and can fly together.
    //
    // The same split was measured on the cancel and replace passes and did nothing - there
    // the order table node was read in the previous pass and is hot, so that loop has only
    // one thing waiting on memory and nothing to overlap it with. The test is "both are
    // waiting on memory", not "split it finer".
    {
        // Loop 1: pull the four fields out of each add and insert it into the order table.
        // Pulling the fields out reads the packet body, which is also memory - but the
        // bodies of this batch were parsed a moment ago and are still hot.
        for (std::uint32_t k = 0; k < self->add_n; ++k) {
            const std::uint8_t* b = self->hit_body[self->bucket_add[k]];
            self->add_o[k] = book::OrderTable::Order{
                itch::read_be<std::uint32_t>(b + itch::kAddSharesOff),
                itch::read_be<std::uint32_t>(b + itch::kAddPriceOff),
                static_cast<std::uint8_t>(b[itch::kAddSideOff] == 'B' ? 0 : 1),
                itch::read_be<std::uint16_t>(b + itch::kLocateOff)};
            self->book.insert_at(itch::read_be<std::uint64_t>(b + itch::kAddRefOff),
                                 self->add_o[k]);
        }
        // Loop 2: only the price levels. The fields were stored in the loop above, so this
        // one does not read a packet body at all.
        for (std::uint32_t k = 0; k < self->add_n; ++k) {
            const book::OrderTable::Order& o = self->add_o[k];
            self->book.level_move(o.sym, o.side, o.price,
                                  static_cast<std::int64_t>(o.shares));
        }
        // Loop 3: only the bookkeeping - the counters, warming the send path, and putting
        // this security on the dirty list.
        // The four things a message carries always come through carried_of: they are stored
        // per packet and are not in the hit_ rows.
        for (std::uint32_t k = 0; k < self->add_n; ++k) {
            const std::uint32_t i = self->bucket_add[k];
            const Carried c = carried_of(self, i);
            note_applied(self, self->add_o[k].sym, c.rx, c.paced, c.measured, c.before,
                         trading);
        }
    }
    // Pass 2 - the half of a replace that creates. The side is not on the wire, so it is left
    // empty for now and the slot number is written down.
    // Every replace has a different new order id, so all the accesses can be in flight
    // together. The price levels are not touched: the side is not known yet.
    for (std::uint32_t k = 0; k < self->repl_n; ++k) {
        const std::uint8_t* b = self->hit_body[self->bucket_repl[k]];
        const book::OrderTable::Order o{
            itch::read_be<std::uint32_t>(b + itch::kReplaceSharesOff),
            itch::read_be<std::uint32_t>(b + itch::kReplacePriceOff), 0, 0};
        self->repl_new[k] =
            self->book.insert_at(itch::read_be<std::uint64_t>(b + itch::kReplaceNewRefOff), o);
    }
    // Pass 3 - finding the old order of each replace. It only looks up and changes nothing.
    // It is completely independent: by the time this runs, the old order of every replace
    // exists - either it was resting from an earlier batch or pass 1 or pass 2 just created
    // it. So all the lookups can be in flight together.
    // It also counts how many were not found. That is not a branch: the result of the
    // comparison, 0 or 1, is added.
    std::uint32_t missing = 0;
    for (std::uint32_t k = 0; k < self->repl_n; ++k) {
        const std::uint8_t* b = self->hit_body[self->bucket_repl[k]];
        const std::uint32_t slot =
            self->book.find_slot(itch::read_be<std::uint64_t>(b + itch::kReplaceOldRefOff));
        self->repl_old[k] = slot;
        missing += slot == book::OrderTable::kNoSlot ? 1u : 0u;
    }
    // One test for the whole batch: as long as nothing was missing, which never happens in a
    // clean run, the two passes below carry no branch at all.
    if (missing == 0) {
        // Pass 4 - carrying the side across. This is the only pass that has to follow the
        // order of the stream.
        // But it touches memory not once: both slot numbers were found in passes 2 and 3 and
        // those entries are still in cache.
        // This is where a chain unwinds: the replace before it has already filled in the side
        // of the order it created.
        // In session, thirty to forty percent of replaces are part of a chain and the longest
        // ran to 997 links - so this pass must never be reordered.
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.set_side_sym_at(self->repl_new[k], o.side, o.sym);
        }
        // Pass 5 - settling up: the old order comes off its price level and is really
        // removed, and the new one goes on to its price level. Completely independent, with
        // no branch.
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->repl_old[k]);
            const book::OrderTable::Order fresh = self->book.at(self->repl_new[k]);
            self->book.level_move(fresh.sym, fresh.side, fresh.price,
                                  static_cast<std::int64_t>(fresh.shares));
            const Carried c = carried_of(self, self->bucket_repl[k]);
            note_applied(self, fresh.sym, c.rx, c.paced, c.measured, c.before, trading);
        }
    } else {
        // Only a real packet loss reaches here. This version carries the branches and is ten
        // percent slower, and it runs a handful of times in a whole run.
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            if (self->repl_old[k] == book::OrderTable::kNoSlot) continue;
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.set_side_sym_at(self->repl_new[k], o.side, o.sym);
        }
        for (std::uint32_t k = 0; k < self->repl_n; ++k) {
            if (self->repl_old[k] == book::OrderTable::kNoSlot) {
                // The old order really is not there, so the one pass 2 created has to go: its
                // side can never be filled in and it never reached the price levels. Left in
                // place, something in the next batch would take its false side and use it to
                // reduce a price level.
                self->book.erase_at(self->repl_new[k]);
                continue;
            }
            const book::OrderTable::Order o = self->book.at(self->repl_old[k]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->repl_old[k]);
            const book::OrderTable::Order fresh = self->book.at(self->repl_new[k]);
            self->book.level_move(fresh.sym, fresh.side, fresh.price,
                                  static_cast<std::int64_t>(fresh.shares));
            const Carried c = carried_of(self, self->bucket_repl[k]);
            note_applied(self, fresh.sym, c.rx, c.paced, c.measured, c.before, trading);
        }
    }
    // Pass 6 - the deletes, in three loops.
    // Here "not found" is ordinary rather than exceptional: a replace earlier in this batch
    // may already have taken that order away. So the branch cannot be avoided - but it can be
    // moved into a loop that only touches small arrays.
    {
        // Loop 1: only look up, with the results going into a row. All independent, able to
        // fly together, no branch.
        for (std::uint32_t k = 0; k < self->del_n; ++k) {
            self->look[k] = self->book.find_slot(itch::read_be<std::uint64_t>(
                self->hit_body[self->bucket_del[k]] + itch::kDeleteRefOff));
        }
        // Loop 2: pack the ones that were found into a compact list. There is no branch - the
        // value is written unconditionally and the result of the comparison decides whether
        // the entry counts. This loop touches two small arrays, both in cache.
        // The index from the bucket is packed along with it, because the bookkeeping below
        // needs the arrival time that goes with it.
        std::uint32_t n = 0;
        for (std::uint32_t k = 0; k < self->del_n; ++k) {
            self->keep_slot[n] = self->look[k];
            self->kwant[n] = self->bucket_del[k];
            n += self->look[k] != book::OrderTable::kNoSlot ? 1u : 0u;
        }
        // Loop 3: the actual work, with not one branch in it.
        for (std::uint32_t j = 0; j < n; ++j) {
            const book::OrderTable::Order o = self->book.at(self->keep_slot[j]);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(o.shares));
            self->book.erase_at(self->keep_slot[j]);
            const std::uint32_t i = self->kwant[j];
            const Carried c = carried_of(self, i);
            note_applied(self, o.sym, c.rx, c.paced, c.measured, c.before, trading);
        }
    }
    // Pass 7 - E, C and X reducing shares. Split the same way, and the ones that reach zero
    // are gathered into a list of their own before being removed.
    {
        // Loop 1: only look up, and pull out how much to take off while there. All
        // independent, no branch.
        for (std::uint32_t k = 0; k < self->cut_n; ++k) {
            const std::uint8_t* b = self->hit_body[self->bucket_cut[k]];
            self->look[k] = self->book.find_slot(
                itch::read_be<std::uint64_t>(b + itch::kExecRefOff));
            self->kwant[k] = itch::read_be<std::uint32_t>(b + itch::kExecSharesOff);
        }
        // Loop 2: pack into a compact list, with no branch. Three rows are packed together,
        // because the loop below reads them by the same index.
        std::uint32_t n = 0;
        for (std::uint32_t k = 0; k < self->cut_n; ++k) {
            self->keep_slot[n] = self->look[k];
            self->kwant[n] = self->kwant[k];
            self->zero[n] = self->bucket_cut[k];
            n += self->look[k] != book::OrderTable::kNoSlot ? 1u : 0u;
        }
        // Loop 3: take the shares off the order and off the price level, with no branch.
        // Being asked for more than is left takes what is left - which compiles to the
        // instruction that picks the smaller of two values, not to a branch.
        // The ones that reach zero are noted in look, which is free by now, and really removed
        // in the last loop.
        std::uint32_t z = 0;
        for (std::uint32_t j = 0; j < n; ++j) {
            const book::OrderTable::Order o = self->book.at(self->keep_slot[j]);
            const std::uint32_t off = self->kwant[j] < o.shares ? self->kwant[j] : o.shares;
            self->book.set_shares_at(self->keep_slot[j], o.shares - off);
            self->book.level_move(o.sym, o.side, o.price, -static_cast<std::int64_t>(off));
            const std::uint32_t i = self->zero[j];
            const Carried c = carried_of(self, i);
            note_applied(self, o.sym, c.rx, c.paced, c.measured, c.before, trading);
            self->look[z] = self->keep_slot[j];
            z += o.shares == off ? 1u : 0u;
        }
        // Loop 4: really remove the ones that reached zero, with no branch.
        for (std::uint32_t q = 0; q < z; ++q) self->book.erase_at(self->look[q]);
    }
    self->add_n = self->repl_n = self->cut_n = self->del_n = 0;
}

// The way into the second pass: take whichever path the switch chooses, then clear the
// counts so the next poll starts gathering again.
void apply_hits(Shard* self, bool trading) {
    if (self->group) apply_grouped(self, trading);
    else apply_stream(self, trading);
    self->hit_n = 0;
    // When the message count is cleared the packet count has to be cleared too - the next
    // poll starts again at packet zero.
    self->pkt_n = 0;
}


// Everything a packet costs from beginning to end: from checking its sequence number to the
// last message inside it.
//
// That phrase, the last message inside it, matters: a packet is not one message.
// Measured, a packet often holds a single message, but when things are busy one can hold
// dozens - so what a single call to this function costs varies widely and is not a steady
// number.
// The two ways of running call it differently while calling the same code: on the single
// shard path the card reports and this is called directly; on the sharded path a working
// thread took the packet out of a descriptor first. The difference is only where the packet
// came from; what happens inside is identical.
void take_packet(Shard* self, const std::uint8_t* buf, std::uint32_t len,
                 std::uint64_t hw_ts,
                 const std::unordered_map<std::string, std::uint32_t>* reference,
                 const std::unordered_set<std::string>* wanted,
                 bool trading, const std::atomic<std::uint64_t>* drained) {
    // The seven arguments are: everything this core owns, where the packet starts, how many
    // bytes it is, the arrival time the card stamped on it, the ticker to previous close map
    // read before the run, the hundred and one tickers we trade, whether this run really
    // sends orders, and the replay's own progress - how much it has sent - which deciding
    // whether a window counts needs.
    //
    // This is the main path of the whole program. A packet arrives and the order is: check
    // the sequence number, cut the packet into ITCH messages, work out which stretch of the
    // day each message falls in, update the book, and put the securities touched on the dirty
    // list.
    // It does not work out signals and does not send orders - settle_dirty does that once the
    // batch is done.
    // The question "do we trade this one" comes first, because nine messages in ten are
    // stopped by it, and stopping one saves all the work behind it.
    // Asking costs one byte read from a table indexed by security number, and that table
    // stays in cache.
    const std::uint8_t* traded = self->traded.empty() ? nullptr : self->traded.data();
    // A small test: do we trade this security.
    // Captured by value, which captures the bare pointer above - eight bytes - rather than
    // the whole row; captured by reference, every call would follow one more pointer.
    const auto mine = [=](std::uint16_t sym) {
        // There is a list and this entry is zero, so it is not ours.
        if (traded != nullptr && traded[sym] == 0) return false;
        // An empty list means the whole market, and an entry of one means ours.
        return true;
    // That is the test. It runs for every message, so it is a lambda that the compiler
    // inlines rather than a real function call.
    };
    // This pair of braces is left over: it used to hold the single shard path and the sharded
    // path, and when the sharded one went the braces stayed.
    {
        // Not even long enough for an Ethernet header and a MoldUDP64 header, so it is not a
        // complete market data packet.
        // Dropped without being counted - other broadcast traffic does turn up on the wire
        // occasionally.
        if (len < eth::kHeaderBytes + mold::kHeaderLen) return;
        // Past the Ethernet header is the MoldUDP64 layer.
        // There is no IP header or UDP header to step over here: the card's filter has already
        // selected by address and port, and ef_vi hands over the whole frame from the Ethernet
        // header onwards.
        const std::uint8_t* p = buf + eth::kHeaderBytes;
        // Where the first message of this packet sits in the stream. Removing duplicates and
        // spotting gaps both rest on it.
        const std::uint64_t seq = mold::sequence(p);
        // How many messages this packet holds, which is what the next expected sequence number
        // is worked out from.
        const std::uint16_t count = mold::count(p);

        // The exchange sends the same content twice, one copy down each path, to survive a
        // loss.
        // So the duplicates counted below are not a fault but the design - the number is about
        // half of all packets, and it suddenly falling is what would mean one of the paths has
        // a problem.
        // Removing duplicates goes entirely by sequence number: which path a packet came from
        // does not matter and is never looked at.
        if (!self->started) {
            // The first packet. What its sequence number would be was not known in advance, so
            // it sets the starting point.
            self->started = true;
            // Everything is counted from there.
            self->expect_seq = seq;
        // A number lower than expected: this one has been seen already, on the other path.
        } else if (seq < self->expect_seq) {
            // Counted as a duplicate. The number should be large, about half of all packets,
            // because the exchange sends everything twice.
            ++self->duplicates;
            // Dropped. Returning here matters: handling it again would build the book wrongly.
            return;
        // A number higher than expected: something is missing.
        } else if (seq > self->expect_seq) {
            // Counted as a gap. It has to be zero, and a run where it is not does not count.
            // Nothing is repaired here: the missing messages are gone and the book is wrong
            // from that point on.
            ++self->gaps;
        }
        // Whichever of those happened, the next packet starts here.
        self->expect_seq = seq + count;

        // With the two headers taken off, what is left is a series of ITCH messages.
        const std::size_t payload = len - eth::kHeaderBytes - mold::kHeaderLen;
        // The path that works out the stretch once per packet: before walking the messages,
        // the situation of the whole packet is worked out from the timestamp of its first
        // message, and every message below uses what was worked out,
        // rather than working it out again.
        //
        // Why that is allowed: the four things below - which stretch, whether to send, whether
        // it counts, how long until the open - all describe the situation of the packet
        // holding the message rather than the message itself. Messages inside one packet span
        // about 300 nanoseconds of market time, while a measurement window is a whole second.
        //
        // Why it is worth it: the loop over messages is waiting on memory - the packet body
        // was just written by the card, is not in cache, and the first read really goes to
        // memory. Working out the stretch has nothing to do with that waiting, and left in the
        // same loop it delays every message's memory access by several instructions before it
        // can even be issued.
        //
        // The stretch this packet is in. It starts as kGap and is always overwritten before
        // being used.
        win::Phase pkt_where = win::Phase::kGap;
        // Whether this packet is in the stretch replayed at the real speed.
        bool pkt_paced = false;
        // Whether this packet is inside a measurement window that counts.
        bool pkt_measured = false;
        // If this packet is in a warm up, how many hundredths of a second there are until the
        // window opens.
        std::uint16_t pkt_before = 0;
        // Long enough for a length prefix and the header of the shortest message, or there is
        // no first message to read.
        if (payload >= itch::kLenPrefix + itch::kHeaderLen) {
            // Where the body of the first message starts: past the two byte length prefix.
            const itch::Message head{p + mold::kHeaderLen + itch::kLenPrefix,
                                     static_cast<std::uint16_t>(payload - itch::kLenPrefix)};
            // Its market time stands for the whole packet.
            const std::uint64_t ts = head.timestamp();
            // The opening and closing messages still have to be recognised; they decide where
            // the windows start.
            win::note_session(head, &self->phase);
            // Which stretch of the day this packet falls in.
            pkt_where = self->phase.advance(ts);
            // The bookkeeping for changing stretch below is exactly as it was, only once per
            // packet rather than once per message.
            if (self->every_unit) {
                const std::uint64_t u = self->phase.index();
                if (u != self->at_window) {
                    ++self->windows;
                    self->at_window = u;
                    self->window_ok = true;
                }
            }
            if (pkt_where != self->was) {
                if (pkt_where == win::Phase::kSettle) {
                    self->caught_up = false;
                    self->drain_mark = drained->load(std::memory_order_relaxed);
                } else if (pkt_where == win::Phase::kWindow) {
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
                self->was = pkt_where;
            }
            // Whether this packet may send orders.
            pkt_paced = win::Tracker::one_to_one(pkt_where);
            // Whether this packet's latency samples count.
            pkt_measured = pkt_where == win::Phase::kWindow && self->window_ok;
            // In a warm up, how many hundredths of a second there are until the window opens.
            pkt_before = pkt_where == win::Phase::kSettle
                             ? static_cast<std::uint16_t>(
                                   (self->phase.open() - ts) / 10000000 + 1)
                             : 0;
            // Storing per packet: the four things go into this packet's entry, and each
            // message below records only which packet it is in.
            // The 512 entries are twice what one poll can need, since a poll takes at most 256
            // events; if they filled, nothing more would be stored and the extra messages
            // would point at the last entry and the numbers would be slightly out - but it
            // cannot happen, because 256 is the limit.
            if (self->pkt_n < 512) {
                self->pkt_rx[self->pkt_n] = hw_ts;
                self->pkt_paced[self->pkt_n] = pkt_paced ? 1 : 0;
                self->pkt_measured[self->pkt_n] = pkt_measured ? 1 : 0;
                self->pkt_before[self->pkt_n] = pkt_before;
            }
        }
        // Cut this stretch into messages and hand each to the function below.
        // The return value is ignored: it says whether the walk ran into a length that does
        // not fit, and our own replay sends the file as it is and never produces half a
        // message.
        (void)itch::for_each_message(
            p + mold::kHeaderLen, payload, [&](const itch::Message& m) {
                // Another message, whether or not it is about a security we care about.
                ++self->messages;
                // How many messages this poll has counted. The first one also leaves its
                // market time, which is where this batch sits in the trading day and what any
                // later analysis by time of day rests on.
                if (self->poll_msgs == 0) self->poll_when = m.timestamp();
                // One more for this poll. When the poll finishes, this is merged into the
                // statistics.
                ++self->poll_msgs;
                // The line below asks which stretch of the day this message is in.
                //
                // The book is built from every message whatever the stretch - a book missing a
                // piece stays wrong from that moment until the close.
                //
                // Orders are different: the full speed stretch compresses the market ninety
                // times, and sending there is sending ninety times too fast, which measures
                // not how quick this path is but whether we can keep up with a speed we made
                // up ourselves.
                // Which stretch this message is in was worked out once per packet above and is
                // simply used here.
                // The bookkeeping for the session messages and for changing stretch was done
                // there as well.
                const win::Phase where = pkt_where;
                // Whether this message is in the stretch replayed at the real speed.
                // Only there do orders go out - the full speed stretch compresses the market
                // dozens of times.
                // It is wider than a measurement window: a warm up is 1:1 too and does send
                // orders, they simply do not count towards the latency.
                const bool paced = pkt_paced;
                // Whether an order caused by this message counts towards the report.
                // Two things have to hold: it is inside a measurement window, and that window
                // counts.
                const bool measured = pkt_measured;
                // Messages inside a measurement window are counted whether or not the window
                // counts.
                if (where == win::Phase::kWindow) {
                    // One more message inside a window, which says afterwards which setting
                    // this run used.
                    ++self->window_messages;
                    // If the window does not count, the sample this message leads to is wasted
                    // and is counted as such.
                    if (!self->window_ok) ++self->dropped_samples;
                }
                // Which security this message is about. It is a two byte number rather than a
                // ticker; the correspondence between the two comes from the morning's
                // directory and is sent afresh every day.
                const std::uint16_t sym = m.stock_locate();
                // The stock directory: every morning the exchange sends which number stands
                // for which security, and only then starts sending market data. The numbers
                // are reassigned daily, so yesterday's are all wrong today.
                if (m.type() == 'R') {
                    // Where the eight bytes of the ticker sit in the body.
                    const char* s2 =
                        reinterpret_cast<const char*>(m.body + itch::kStockSymbolOff);
                    // All eight to begin with; the next line cuts the padding off.
                    std::size_t n = itch::kStockSymbolLen;
                    // Trailing spaces removed from the back. The protocol pads a short ticker
                    // with spaces, so "AAPL    " has to become "AAPL" to be found in the table.
                    while (n > 0 && s2[n - 1] == ' ') --n;
                    // This is the moment a name becomes a number, so the list of what we trade
                    // can only be applied here.
                    // And it has to come before the test below, which reads exactly the bytes
                    // written here.
                    // Only a non empty list needs marking; an empty one means the whole market
                    // and there is nothing to mark.
                    if (!self->traded.empty()) {
                        // Ask the list about this ticker and mark one or zero.
                        // A std::string is built here, which allocates - but this is a
                        // directory message before the open, some ten thousand of them in a
                        // day, and not on the hot path.
                        self->traded[sym] =
                            wanted->count(std::string(s2, n)) != 0 ? 1 : 0;
                    }
                    // Not one of the securities we trade, so there is no price to bind.
                    // The true tells the walk to carry on to the next message; it does not
                    // mean success.
                    if (!mine(sym)) return true;
                    // Stored as the eight raw bytes an order needs, so that no string is
                    // touched once the run is going.
                    std::memcpy(&self->symbols[sym * 8], s2, itch::kStockSymbolLen);
                    // Look this security up in the previous close table.
                    const auto it = reference->find(std::string(s2, n));
                    // A security with no reference price gets no price space and can never
                    // produce a signal.
                    // It has to be counted: with a hundred and one names, one missing is a
                    // percent of the samples.
                    if (it != reference->end() && self->book.bind(sym, it->second)) {
                        // The price space exists and this security can now produce signals.
                        ++self->bound;
                    // Either there was no reference price, or the price space could not be cut
                    // because the block ran out.
                    } else {
                        // Counted, because with a hundred and one names one missing is a
                        // percent of the samples.
                        ++self->unbound;
                    }
                    // The directory message is dealt with; on to the next.
                    return true;
                }
                // Not one of the securities we trade, so it is skipped -
                // nine messages in ten stop here, and stopping one saves all the work behind
                // it.
                if (!mine(sym)) return true;
                // If this message is in a warm up, how many hundredths of a second there are
                // until the window opens; zero otherwise.
                // Both paths need it, so it is worked out first.
                const std::uint16_t before = pkt_before;
                // From this line on is the border between the first pass and the second.
                // False: a message is applied to the book as soon as it is parsed, in one
                // pass.
                // True: only where it is and how long it is are recorded, and the book is
                // built once the batch has been collected, in two passes.
                // Without --split-ab, collect is always true and the second is what runs.
                if (!self->collect) {
                    // Which security's book this message changed, filled in by apply.
                    std::uint16_t touched = 0;
                    // The line that really builds the book: adds, cancels, executions and
                    // replaces all happen inside it.
                    // A false means the message did not change the book - for instance it
                    // mentioned an order id we have never seen - and then there is nothing to
                    // record.
                    if (self->book.apply(m, &touched)) {
                        // The bookkeeping: the counters, warming the send path, and putting
                        // this security on the dirty list.
                        note_applied(self, touched, hw_ts, paced ? 1 : 0,
                                     measured ? 1 : 0, before, trading);
                    }
                    // This message is dealt with; on to the next.
                    return true;
                }
                // Full, so what is in hand is processed first and gathering starts again.
                // This cannot happen now - the arrays are sized exactly for what one poll can
                // gather.
                // It stays as a trip wire: if somebody raises the limit of a poll and misses
                // this, it catches it.
                if (self->hit_n == kMaxHits) apply_hits(self, trading);
                // The seven pass path needs the messages bucketed by type. A bucket holds the
                // position of a message rather than the message, because the three rows below
                // already record everything needed.
                // The one at a time path does not bucket, so this is asked first.
                if (self->group) {
                    // Four buckets by type. These four, because what they do to the book
                    // differs: adds only add, deletes only take away, executions and cancels
                    // take part of an order away, and a replace adds one and removes another.
                    switch (m.type()) {
                        // An add: a new resting order. F is an add carrying the broker's
                        // number, and it does the same thing to the book.
                        case 'A': case 'F': self->bucket_add[self->add_n++] = self->hit_n; break;
                        // A replace: one message doing two things - adding under the new order
                        // id and removing the order with the old one.
                        case 'U': self->bucket_repl[self->repl_n++] = self->hit_n; break;
                        // Traded or partly cancelled: some shares come off an existing order.
                        case 'E': case 'C': case 'X': self->bucket_cut[self->cut_n++] = self->hit_n; break;
                        // A cancel: the whole order goes.
                        case 'D': self->bucket_del[self->del_n++] = self->hit_n; break;
                        // No other type reaches here - they were stopped further up.
                        default: break;
                    }
                }
                // The three rows below say where a message is, one entry each, indexed by
                // hit_n.
                // A pointer rather than the content: the packet body stays where it is in the
                // receive buffer and is read once the batch has been collected.
                //
                // Only three things are written. It used to be four more - the arrival time,
                // whether in the 1:1 stretch, whether in a window, how long until the open -
                // twenty two bytes spread over six arrays; all four describe the situation of
                // the packet holding the message, so they are now stored once per packet in
                // the pkt_ rows above and each message records only which packet it is in,
                // which is twelve bytes over three rows.
                //
                // Why that is worth having: this loop is waiting on memory - the packet body
                // was just written by the card, and the first read really goes to memory, some
                // 140 nanoseconds. Adding work to a loop that waits on memory costs ten times
                // what adding it to an arithmetic loop costs, because those instructions sit
                // in the middle of the chain "read the length, work out the next address, read
                // again" and delay the next access being issued.
                //
                // Which byte this message starts at.
                self->hit_body[self->hit_n] = m.body;
                // How long it is.
                self->hit_len[self->hit_n] = m.len;
                // Which packet of this batch it is in. The four things it carries are fetched
                // from that packet's entry.
                self->hit_pkt[self->hit_n] = self->pkt_n;
                // One more gathered.
                ++self->hit_n;
                return true;
        // This line closes two things at once: the brace closes the function above that
        // handles one message, and the parenthesis closes this call to for_each_message. The
        // packet has been cut up by the time it is reached.
            });
        // This packet is finished, so the packet number moves on and the messages of the next
        // packet record a new one.
        if (self->pkt_n < 512) ++self->pkt_n;
    }
}

// That is the end of the unnamed namespace: the names above are invisible outside this file.
}  // namespace

// Where the program starts.
//
// Fourteen hundred lines looks like a tangle. It is in fact straight - one line from top to
// bottom with no branching about, in five parts:
//   1 take the command line, read the reference prices and the list of securities
//   2 open two ef_vi interfaces, one for market data and one for acknowledgements, install
//     the filters, post the buffers
//   3 work out how large the order table and the price space have to be, get it all at once,
//     then touch every page
//   4 shake hands, start the acknowledgement thread, then enter the loop that runs forever
//   5 finish: stop the threads, write the samples out as a few csv files, print the numbers
//     that have to add up
// Only the loop in part four is the hot path; parts one to three and part five run once and
// may be as slow as they like.
int main(int argc, char** argv) {
    // Everything adjustable takes its default first, and the loop below overrides from the
    // command line.
    Options opt;
    // From the first argument, because the zeroth is the program's own name.
    // The argv[++i] inside the loop skip a place of their own - past the value a switch
    // carries - so this for's ++i and the ones inside work together rather than repeating.
    for (int i = 1; i < argc; ++i) {
        // Whether this switch is followed by a value.
        // Every switch that takes one asks this first, or argv[++i] would read past the end.
        const bool has = i + 1 < argc;
        // The argument being looked at, which the whole run of strcmp below compares against.
        const char* a = argv[i];
        // What follows is a long run of command line switches. They are far easier to read in
        // five groups:
        //
        //   1 what this run is about   --reference / --symbols / --dst-ip / --dst-port(-b)
        //     Getting these wrong reports nothing and quietly produces worthless numbers.
        //     Changing the day means changing --reference and the data file together; missing
        //     one only moves every place the strategy fires.
        //     Leaving out --symbols takes the universe from a hundred and one names to the
        //     whole market, and the p50 from three thousand nanoseconds to ten milliseconds.
        //
        //   2 where it runs           --intf / --cpu-base / --idle-ms
        //
        //   3 where the orders go     --order-ip / --order-port / --local-ip
        //     Without --order-ip the run only works out signals and sends nothing at all.
        //
        //   4 the strategy            --threshold
        //
        //   5 where the results go    --out for the latency samples, --stat for a line per
        //     poll, which is for diagnosis
        //
        // The port name. The default is the Solarflare one.
        if (std::strcmp(a, "--intf") == 0 && has) opt.intf = argv[++i];
        // The csv of reference prices, which is tied to the data file.
        else if (std::strcmp(a, "--reference") == 0 && has) opt.reference = argv[++i];
        // Subscribe only to the tickers in this file. Leaving it out means the whole market,
        // as above.
        else if (std::strcmp(a, "--symbols") == 0 && has) opt.symbols = argv[++i];
        // Which multicast address the market data goes to.
        // It decides which packets the card delivers to our interface - it is what the filter
        // below is built from.
        else if (std::strcmp(a, "--dst-ip") == 0 && has) {
            // A failure to parse exits at once. With the address wrong the filter installs
            // and receives nothing, and all that can be seen is market data that never
            // arrives.
            if (!parse_ip(argv[++i], &opt.dst_ip)) return 2;
        // The destination port of the market data. With the address above it decides what the
        // filter selects.
        } else if (std::strcmp(a, "--dst-port") == 0 && has) {
            // The third argument of strtoul, 10, means read it as decimal.
            // Whether it succeeded is not checked: the command line is ours and is not
            // defended against.
            opt.dst_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        // The port of the second market data path. The exchange sends the same content twice
        // and we take both, removing duplicates by sequence number.
        } else if (std::strcmp(a, "--dst-port-b") == 0 && has) {
            // Left out it is 0, and installing the filters below skips the second path.
            opt.dst_port_b = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        // Which core to start pinning at. Polling, acknowledgements and processing follow in
        // order.
        // It also decides which half of the memory the huge pages come from - see huge::choose
        // above.
        } else if (std::strcmp(a, "--cpu-base") == 0 && has) {
            // atoi because a core number may be negative, which means "do not pin this
            // thread".
            opt.cpu_base = std::atoi(argv[++i]);
        // The signal's threshold, as a percentage. The default is 75.
        } else if (std::strcmp(a, "--threshold") == 0 && has) {
            // Comparing the latency of two runs means this number has to match: it decides
            // directly how many orders go out.
            opt.threshold = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        // Make the polls take turns between the two ways of building the book, timing each.
        // See the note in Options.
        // Back to the old way: build the book one message at a time in the order of the
        // stream. Only passed when comparing the two.
        } else if (std::strcmp(a, "--one-at-a-time") == 0) {
            opt.group = false;
        } else if (std::strcmp(a, "--split-ab") == 0) {
            opt.split_ab = true;
        // Write the raw figures of every poll to polls_raw.csv in this directory.
        // scripts/poll_stat.py reads it.
        } else if (std::strcmp(a, "--stat") == 0 && has) {
            // With this on, every poll records a line, and sixty million records take hundreds
            // of megabytes.
            // It stays off normally and goes on only when the question is how much work a poll
            // has in hand.
            opt.stat = argv[++i];
        // Print only the figures for the segments and skip the large per poll file.
        } else if (std::strcmp(a, "--segments") == 0) {
            opt.segments = true;
        // Our own address, which our own connection needs to fill in the IP header.
        } else if (std::strcmp(a, "--local-ip") == 0 && has) {
            // It has to match what the card's filter was given, or the acknowledgements coming
            // back are never received.
            opt.order_local = argv[++i];
        // How long without a packet before the replay is taken to have finished, which is what
        // ends a run by itself.
        } else if (std::strcmp(a, "--idle-ms") == 0 && has) {
            // strtoull rather than strtoul because this field is sixty four bits.
            opt.idle_ms = std::strtoull(argv[++i], nullptr, 10);
        // Which machine the orders go to. Without it the run only works out signals and sends
        // nothing at all.
        } else if (std::strcmp(a, "--order-ip") == 0 && has) {
            // Only the string is kept; turning it into a number happens where the
            // acknowledgement interface is opened.
            opt.order_ip = argv[++i];
        // Which port the exchange side is listening on for orders.
        } else if (std::strcmp(a, "--order-port") == 0 && has) {
            // It has to match the port the exchange side listens on, and the one given to the
            // TCP filter below.
            opt.order_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
        // Which directory the samples are written to when the run finishes.
        // latency.csv, events.csv and polls.csv all land there.
        } else if (std::strcmp(a, "--out") == 0 && has) {
            // Only the path is kept; the directory is created when the files are written at
            // the end.
            opt.out = argv[++i];
        // Nothing above matched, so it is either a misspelt switch or a switch that takes a
        // value with nothing after it.
        } else {
            // An argument that is not recognised. It is not ignored so the run can carry on -
            // a misspelt switch would quietly run this round on a different configuration, and
            // that is the hardest kind of mistake to find.
            std::fprintf(stderr,
                         "usage: trader [--intf I] [--reference CSV] [--symbols FILE]\n"
                         "              [--dst-ip A.B.C.D] [--dst-port N] [--dst-port-b N]\n"
                         "              [--cpu-base N] [--threshold N] [--idle-ms N]\n"
                         "              [--order-ip A.B.C.D] [--order-port N]\n"
                         "              [--local-ip A.B.C.D] [--out DIR] [--stat DIR]\n"
                         "              [--split-ab] [--one-at-a-time]\n"
                         "  ITCH_SKIP_WINDOWS / ITCH_MAX_WINDOWS pick which windows count\n"
                         "  sending orders needs onload in front of it\n");
            // A 2 means the usage was wrong, which is kept apart from the 1 of a run that
            // cannot start.
            return 2;
        }
    }

    // The reference prices, in two shapes: one to look up by ticker, one in a plain row.
    std::unordered_map<std::string, std::uint32_t> reference;
    // The same prices in a row. Working out how large the price space has to be adds them up
    // one security at a time below, and walking a row is far quicker than going round a hash
    // table - and this happens once.
    std::vector<std::uint32_t> prices;
    // Unreadable means exiting: with no reference prices, not one security gets a price space
    // and not one order can go out.
    // Forgetting to change this file when changing the day is readable but wrong, and that
    // mistake cannot be caught here - only by looking at how many orders went out afterwards.
    if (!read_reference(opt.reference, &reference, &prices)) {
        // The path is printed, because a wrong path or a forgotten day are the usual causes.
        std::fprintf(stderr, "cannot read %s\n", opt.reference);
        // A 1 means the run cannot start. With no reference prices not one security gets a
        // price space.
        return 1;
    }

    // Which securities we subscribe to.
    //
    // This file can only hold names, not numbers.
    // The security number in a message is reassigned by the exchange every morning, so a list
    // of numbers is only right for one day and is misaligned on any other.
    //
    // What happens without this argument: the universe goes from a hundred and one names to
    // the whole market's eleven thousand and more, six times as many orders go out, and the
    // tick to trade p50 goes from three thousand nanoseconds to ten milliseconds.
    // And nothing reports it - the only way to notice is the universe line in the log after
    // the run starts.
    std::unordered_set<std::string> wanted;
    // There are two kinds of empty here and they mean opposite things:
    //   --symbols was not given    -> the whole section is skipped, wanted stays empty -> the
    //                                 whole market
    //   it was given but the file names nothing -> the code below exits rather than treating
    //                                 it as the whole market
    // The second has to be stopped, because the user plainly meant "only these few".
    // Without the argument the whole section is skipped and wanted stays empty - an empty set
    // means the whole market.
    if (opt.symbols != nullptr) {
        // Open the list. It is plain text, one ticker per line, with no header.
        std::FILE* f = std::fopen(opt.symbols, "r");
        // Unable to open means exiting. It must not be treated as "the argument was not
        // given", which would quietly subscribe to the whole market.
        if (f == nullptr) {
            // The path is printed, because a wrong path is the usual cause.
            std::fprintf(stderr, "cannot read %s\n", opt.symbols);
            // Exit with 1.
            return 1;
        }
        // Sixty four bytes is plenty - a ticker is at most eight characters.
        char line[64];
        // A line at a time to the end of the file.
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            // Copied into a std::string so the tail can be trimmed below.
            // This allocates, but it happens a hundred and one times before the run and is not
            // on the hot path.
            std::string s(line);
            // Newlines, carriage returns and spaces are trimmed from the end.
            // All three matter: this file may have been edited on Windows, and one stray
            // carriage return would keep that ticker from ever matching.
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
                // Drop the last character and test again.
                s.pop_back();
            }
            // Still empty after trimming is skipped, which is how the blank line at the end of
            // a file is filtered out.
            if (!s.empty()) wanted.insert(s);
        }
        // The list has been read, so the file is closed.
        std::fclose(f);
        // The file was there but not one name came out of it.
        // This must not be treated as "do not filter" - that would subscribe to the whole
        // market when the user plainly gave the argument. So it exits.
        if (wanted.empty()) {
            // The file name is printed. The usual causes are an empty file or the wrong
            // format.
            std::fprintf(stderr, "%s named no securities\n", opt.symbols);
            // Exit with 1 as well.
            return 1;
        }
        // Narrow the reference prices from the whole market's twelve thousand names down to
        // the hundred and one we want.
        //
        // Why it has to be narrowed: how much memory to reserve is worked out from this row of
        // prices, and that space is cut per security. Unnarrowed it would be cut for twelve
        // thousand - about two gigabytes of huge pages, every page of which then has to be
        // touched before the run, all of it wasted. Narrowed it is 0.21 GB.
        //
        // Names in the list with no reference price are not dealt with here: they take up
        // little room, and how many did not get a price space is reported plainly later, which
        // is better than guessing here.
        std::vector<std::uint32_t> keep;
        // Every ticker in the list is looked up in the reference prices.
        for (const auto& n : wanted) {
            // Does this ticker have a previous close.
            const auto it = reference.find(n);
            // Only a price that was found is kept; one that was not is left alone, and how
            // many got no price space is reported later.
            if (it != reference.end()) keep.push_back(it->second);
        }
        // This line has to be looked at once a run starts. It is the only evidence of how many
        // securities this round subscribed to - forgetting --symbols prints ten thousand and
        // more here, and every number from that round is worthless.
        std::printf("universe       %zu names, %zu of them with a reference price\n",
                    wanted.size(), keep.size());
        // The narrowed row replaces the whole market's row.
        // swap rather than assignment: assignment copies thousands of numbers, while swap
        // exchanges two pointers.
        prices.swap(keep);
    }

    // These two lines have to come before the card's buffers are mapped, not after.
    //
    // Those buffers are huge pages too, and unless told otherwise a huge page comes from
    // whichever half this thread happens to be on.
    // The memory of this machine is in two halves, and reaching across to the other takes more
    // than three times as long.
    // Both the card and the working thread are downstream of this choice, so it has to be made
    // first.
    pin(opt.cpu_base);
    // Tell the huge page allocator which half to take from.
    // node_of_cpu is which half a core belongs to.
    huge::choose(huge::node_of_cpu(opt.cpu_base));
    // Printed. If the two do not agree - the core on one side and the memory on the other -
    // the whole round reaches across for every access and is three times slower, and the
    // numbers themselves give no hint why.
    std::printf("huge pages     taken from node %d, the one cpu %d is on\n",
                huge::node_of_cpu(opt.cpu_base), opt.cpu_base);

    // From here on is all the preparation of the ef_vi interface that receives market data.
    //
    // Set against socket programming first, so it is clear what each step replaces:
    //   socket()                  -> vi.open              open an interface
    //   bind()                    -> ef_filter_*          decide which packets are ours
    //   the kernel owns the receive buffers -> ef_vi_receive_init  we post them ourselves
    //   recv()                    -> ef_eventq_poll       take the events; the data stays put
    //   send()                    -> ef_vi_transmit       hand over, with no copy and no wait
    // The biggest difference is the third line: in socket programming where the buffers come
    // from is never a question, while here we allocate them, register them, post them and take
    // them back ourselves.
    //
    // There are five steps and the order cannot change, because each uses what the one before
    // it produced: without an interface there is nothing to set an entry size on, without
    // registered memory there is no address to post, and without a filter the card does not
    // know who a packet belongs to.
    //   1 open the interface     (the vi.open below)
    //   2 say how large an entry is   (ef_vi_receive_set_buffer_len)
    //   3 get a block of DMA memory   (frames.alloc)
    //   4 install the filters    (ef_filter_*, which decides what enters our queue)
    //   5 post the buffers       (ef_vi_receive_init and push, some lines further down)
    // Only after all five does the card start writing packets into our memory.
    ef::Vi vi;
    // Step one: open the interface. It is the equivalent of socket(), except the depth of
    // every ring is ours to state.
    // The four arguments are the port name, how many entries the receive ring has, how many
    // the transmit ring has, and the flags.
    //
    //   a receive ring of 4096
    //     It looks like "at most 4096 packets can be buffered". It is not.
    //     An entry holds the address of one buffer. The number was measured: 4096 works and
    //     8192 fails.
    //     It is how many buffers the card knows about at once, not how many packets can be
    //     buffered - what really decides the latter is the 4 GB block below.
    //   a transmit ring of 0
    //     This interface only receives. Orders go out on another one, see open_order_path.
    //     Why they are apart: the event queue of an interface allows only one consumer, and
    //     market data and acknowledgements are polled on different threads.
    //   EF_VI_RX_TIMESTAMPS
    //     Makes the card write a short piece in front of every packet received, holding the
    //     hardware moment that packet reached the wire.
    //     It is where the whole wire to wire measurement starts - without this flag there is
    //     no measurement at all.
    //     The cost is that short piece in front of every packet, which reading a packet has to
    //     step over later, see prefix.
    if (!vi.open(opt.intf, static_cast<int>(cfg::kRxDescriptors), 0, EF_VI_RX_TIMESTAMPS)) {
        // It would not open. The usual causes are a wrong port name, this card having run out
        // of interfaces, or not running inside the network namespace.
        return 1;
    }
    // Step two: tell the card how large one of our entries is, at 512 bytes.
    // Without being told it uses its default, which may be larger than our entry - and then a
    // long packet would be written into the next entry, over a packet not yet read.
    ef_vi_receive_set_buffer_len(vi.get(), cfg::kRxSlotBytes);
    // Step three: get that 4 GB of huge pages, cut into eight million entries, and register it
    // with the card.
    // Registering matters: an address that was never registered is blocked by the address
    // translation hardware when the card touches it.
    ef::Frames frames;
    // Unable to get it means exiting. The usual causes are a huge page pool that was not
    // cleared after a previous run, or not enough memory.
    if (!frames.alloc(vi, cfg::kRxRingSlots, cfg::kRxSlotBytes)) return 1;

    // Step four: install the filters, which is the equivalent of bind() in socket programming.
    //
    // This step is where bypassing the kernel really happens, and it is worth spelling out.
    // Having received a packet, the card has to decide who to give it to.
    // Installing a filter tells the card: a packet matching this does not go to the kernel, it
    // goes straight into my interface.
    // After that, as far as the kernel is concerned those packets never reached this machine -
    // so there is no interrupt, no protocol stack, no socket, and not one copy of the memory.
    //
    // The exchange sends the same market data twice, on two multicast ports, so two filters
    // are installed.
    for (std::uint16_t port : {opt.dst_port, opt.dst_port_b}) {
        // A port of 0 means that path was not configured, so it is skipped. Running a single
        // path reaches this.
        if (port == 0) continue;
        // A description of what sort of packet is wanted. It is only a local structure until
        // it is handed to the card.
        ef_filter_spec fs;
        // Initialised first. This line cannot be skipped - the structure is on the stack and
        // holds rubbish until it is.
        // EF_FILTER_FLAG_NONE asks for no extra behaviour.
        // (Of the others the common ones are "take all multicast" and "let several interfaces
        //  share one filter"; neither is wanted here - one interface owns it, and only the
        //  multicast address named is wanted.)
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        // The conditions: protocol UDP, destination address that multicast address,
        // destination port this path's port.
        // local means only the destination side is looked at, whoever sent the packet.
        // Multicast has no fixed source anyway, so this is the only way to select it.
        // Both the address and the port are turned into network order, because the card
        // compares them as they appear on the wire.
        int rc = ef_filter_spec_set_ip4_local(&fs, IPPROTO_UDP, htonl(opt.dst_ip),
                                              static_cast<int>(htons(port)));
        // Once installed the card returns a token, which is what removing this filter later
        // would need.
        // Ours stay until the process ends, so it is never used.
        ef_filter_cookie cookie;
        // This is what really hands it to the card. Once this line has run, matching packets
        // start arriving.
        // Both failures need looking at: the conditions were wrong (rc < 0), or the card's
        // filter table is full.
        if (rc < 0 || (rc = ef_vi_filter_add(vi.get(), vi.dh(), &fs, &cookie)) < 0) {
            // The port and the error code are both printed, so it is clear which path and
            // which step failed.
            std::fprintf(stderr, "cannot listen on port %u: %d\n", port, rc);
            // Exit with 1. A round that receives no market data is pointless.
            return 1;
        }
    }

    // This connection is ours, so its packets have to land here rather than in the kernel -
    // the kernel has no socket for them and would answer with a connection reset.
    //
    // The filter here uses all four numbers, the address and port of both ends, rather than
    // the port alone: that takes only this one connection and leaves anybody else's traffic on
    // the same port alone.
    // The two lines below are printed every round and say which path this round takes.
    // They have a history: the quick path had long been merged and tagged as a baseline, but
    // it was off by default and the run script did not pass it - so a whole day of comparisons
    // ran on the slow path it replaces, and that path has an occasional stall of tens of
    // microseconds which completely drowned the two microseconds being measured.
    // With these lines, a wrong setting is plain in the log.
    std::printf("signal         once per poll, %s\n",
                "and only when the imbalance moved further that way");
    // The second line: orders go out over our own TCP.
    std::printf("order path     %s\n",
                "our own TCP");
    // The third line: which way the book is built. It has to be here - taking the wrong path
    // shows up nowhere else, and one switch that was not passed once cost a whole day of
    // comparisons.
    std::printf("book path      %s\n",
                opt.group ? "grouped in seven passes" : "one message at a time");
    // The addresses of both ends, parsed below from the two strings on the command line.
    // Installing the TCP filter needs them, and so does building the IP header of every packet.
    std::uint32_t peer_ip = 0, local_ip = 0;
    // Acknowledgements get a queue of their own rather than sharing the market data one.
    //
    // Why they are apart: which queue the card puts a packet into is decided by the filter,
    // and a queue is first in, first out. With two filters on one queue, an acknowledgement
    // coming back from the exchange queues behind the market data - and in a burst there are
    // thousands of packets in the queue, so the acknowledgement that would free the send
    // window sits at the bottom and we have to chew through the market data to reach it.
    // Until then "how much they have received" does not move, the bytes not yet acknowledged
    // only grow, and once they hit the limit not one order can go out.
    //
    // Market data is UDP and the order connection is TCP, and the five values that identify
    // them differ completely, so the card is quite willing to put them in two queues - what
    // cannot be split is one flow, not two different ones.
    // A second queue on the same card is enough; a second port is not needed.
    ef::Vi ack_vi;
    // Its block of memory registered with the card.
    ef::Frames ack_frames;
    // Acknowledgements are small and few: a few thousand a second, sixty odd bytes each. So
    // the ring is far smaller than the market data one and the memory saved goes to the book.
    // This queue asks for no arrival timestamps - when an acknowledgement arrived is not
    // measured, only how far it moved "how much they have received". One timestamp fewer is
    // also one event fewer.
    constexpr std::size_t kAckRingSlots = 512;
    // 2048 bytes an entry. An acknowledgement is sixty odd bytes, and this size is simply
    // convenient - 512 entries of 2048 is 1 MB and not worth being careful about.
    constexpr std::size_t kAckSlotBytes = 2048;
    // How many bytes the card writes in front of each packet. It can only be asked once the
    // interface is open, so this holds the place.
    std::size_t ack_prefix = 0;
    // Which entry is posted back next. Filling the ring leaves the last entry empty, so it
    // starts there.
    // The handshake uses it first, and afterwards the acknowledgement thread carries on with
    // it.
    std::size_t ack_post = kAckRingSlots - 1;
    // This pair of braces only fences the section off so the temporaries inside do not outlive
    // it. It does nothing else.
    {
        // Three things all have to hold: the far end's address was given, it parses, and our
        // own address parses too.
        // Those two parse_ip calls have a side effect - they write their results into peer_ip
        // and local_ip.
        if (opt.order_ip == nullptr || !parse_ip(opt.order_ip, &peer_ip) ||
            !parse_ip(opt.order_local, &local_ip)) {
            // One line covers all three: no far end address, a far end address that will not
            // parse, and our own that will not parse.
            std::fprintf(stderr, "own tcp needs both addresses\n");
            // Exit with 1; this round cannot start.
            return 1;
        }
        // A second ef_vi interface on the same port.
        //
        // One card can carry many interfaces at once, each with its own rings and event queue.
        // They do not disturb one another: a packet of one interface never turns up in
        // another's queue.
        // Which one it goes to is decided entirely by the filters installed below.
        //
        // Why a second one is necessary: the event queue of an interface allows only one
        // consumer.
        // Market data is polled by the polling thread and acknowledgements by the
        // acknowledgement thread.
        // Two threads cannot watch one queue.
        // Splitting them took the orders turned away to zero - the figures are with take_ack.
        //
        // EF_VI_FLAGS_DEFAULT asks for no flags at all.
        // In particular no arrival timestamps: when an acknowledgement arrived is not
        // measured, only how far it moved "how much they have received".
        // One timestamp fewer means a shorter prefix on every packet and one event fewer.
        if (!ack_vi.open(opt.intf, static_cast<int>(kAckRingSlots), 0,
                         EF_VI_FLAGS_DEFAULT)) {
            // The second interface would not open. A card carries a limited number, and a
            // failure here usually means they have run out.
            return 1;
        }
        // As with the market data one: tell the card how large an entry is.
        ef_vi_receive_set_buffer_len(ack_vi.get(), kAckSlotBytes);
        // Get memory and register it with the card. This interface needs only 512 entries,
        // far fewer than the market data one.
        if (!ack_frames.alloc(ack_vi, kAckRingSlots, kAckSlotBytes)) return 1;
        // Ask how many bytes the card writes in front of each packet.
        //
        // That piece is called the prefix. It is not part of the packet but something the card
        // puts there itself - with arrival timestamps on, for instance, the moment is written
        // there.
        // So reading a packet means stepping over that many bytes first to reach the real
        // Ethernet header.
        // Forgetting to step over it parses rubbish, and nothing reports it.
        //
        // Its length depends on which flags an interface was opened with, so every interface
        // has to be asked separately.
        // This one has no timestamps and the market data one does, so their prefixes differ.
        ack_prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(ack_vi.get()));
        // Another description of what sort of packet is wanted. It is the same type as the
        // market data one, but what goes into it is completely different - that one selects
        // UDP multicast, this one selects one TCP connection.
        ef_filter_spec fs;
        // Initialised first as before, or the structure holds rubbish from the stack.
        ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
        // Our own port. One per shard, counting up from 51000.
        const std::uint16_t lp = static_cast<std::uint16_t>(51000 + 0);
        // The far end's port, one per shard as well.
        const std::uint16_t rp = static_cast<std::uint16_t>(opt.order_port + 0);
        // The token the card gives back, which only removing the filter would need. Ours stays
        // until the process ends.
        ef_filter_cookie ck;
        // This is the full form rather than the local form the market data used.
        // The difference is whether the source is looked at:
        //   local  only the destination address and port. Multicast uses this - who sent it
        //          does not matter.
        //   full   the address and port of both ends, all four.
        //          A TCP connection is identified by exactly those four, which is why it is
        //          used here.
        //
        // Why it has to be precise to all four: the packets of this connection belong to the
        // kernel. Taking them means taking only this one - taking more would take somebody
        // else's TCP on the same port. And taking too few lets a packet reach the kernel,
        // which finds no socket for it and answers with a reset that kills our own connection.
        if (ef_filter_spec_set_ip4_full(&fs, IPPROTO_TCP, htonl(local_ip),
                                        static_cast<int>(htons(lp)),
                                        htonl(peer_ip),
                                        static_cast<int>(htons(rp))) < 0 ||
            ef_vi_filter_add(ack_vi.get(), ack_vi.dh(), &fs, &ck) < 0) {
            // A failure here means not one order can go out - the acknowledgements coming back
            // are never received and the send slots are never freed.
            std::fprintf(stderr, "cannot listen for the order connection\n");
            // Exit with 1: with no reference prices not one security gets a price space.
            return 1;
        }
        // Step five: post the buffers into the receive ring, which is what finally gives the
        // card somewhere to write.
        //
        // Socket programming has no such step - the kernel keeps a pool of buffers and all you
        // do is recv. Here that pool is gone and where the buffers come from, how many are
        // posted and when they go back are all ours.
        // It is also what makes something like "an entry is written again only eight million
        // packets later" possible at all.
        //
        // Posting buffers comes in pairs:
        //   ef_vi_receive_init  writes one entry into the ring. It only writes our own memory
        //                       and the card cannot see it yet.
        //   ef_vi_receive_push  presses the doorbell once, telling the card about everything
        //                       written before it.
        // So the right way to use them is many inits and one push.
        // Pressing the doorbell for every entry crosses the bus every time, for nothing.
        //
        // The three arguments of ef_vi_receive_init:
        //   the interface, the address of this entry as the card sees it, and the number we
        //   give the entry.
        // That number is the important part: the card knows nothing about our pointers, and on
        // receiving a packet it hands the number back (see EF_EVENT_RX_RQ_ID in an event). What
        // number goes with which entry is entirely ours to decide, and here it is the index.
        //
        // Leaving the last entry unfilled is how this code always does it: what goes back is
        // always a different entry from the one just read, so an entry is written again only
        // after a long time.
        for (std::size_t k = 0; k + 1 < kAckRingSlots; ++k) {
            // Write one entry into the ring. A failure exits - a ring that cannot be filled
            // leaves the receive side crippled.
            if (ef_vi_receive_init(ack_vi.get(), ack_frames.dma(k), k) < 0) return 1;
        }
        // All five hundred odd entries told to the card at once, which is the many inits and
        // one push above.
        // Only once this line has run does the card really have somewhere to write this
        // connection's packets.
        ef_vi_receive_push(ack_vi.get());
    }

    // The prefix length of the market data interface.
    // The prefix looks like a fixed value that need only be asked for once. It is not - it
    // depends on which flags an interface was opened with, so every interface has to be asked
    // separately.
    // This one has arrival timestamps on, so its prefix carries that moment.
    // Every packet read later steps over this many bytes.
    const std::size_t prefix = static_cast<std::size_t>(ef_vi_receive_prefix_len(vi.get()));
    // Which buffer is posted next. It counts up all the way to eight million before wrapping,
    // which is how "an entry is written again only after a long time" is done.
    std::size_t next_post = 0;
    // Fill the receive ring, 4096 entries less the last one.
    // What is posted here is the first four thousand odd of the eight million. The vast
    // majority are spare, and a buffer that goes back after a packet has been read comes from
    // the spares rather than being the one just read.
    for (; next_post < cfg::kRxDescriptors - 1; ++next_post) {
        // Write one entry into the ring: the address of this buffer as the card sees it, and
        // the number we give it, which is the index.
        // A failure exits - a ring that cannot be filled leaves the receive side crippled, and
        // that kind of crippling is never reported.
        if (ef_vi_receive_init(vi.get(), frames.dma(next_post), next_post) < 0) return 1;
    }
    // Press the doorbell once. Only after this line does the card really start writing packets
    // into our memory.
    ef_vi_receive_push(vi.get());

    // How large the order table is. It used to be a fixed 12.58 million entries, on the
    // grounds that the whole market peaks at 6.68 million resting orders with 1.88 times over.
    // But that is the whole market's number: with only a hundred and one names, the measured
    // peak is 1.525 million, which means eighty seven percent of the entries were never used.
    //
    // The good of a smaller table is not the memory saved but whether it and the price space
    // together fit in the third level cache.
    // This machine has 384 MB of it, and the old order table's 436 MB plus the price space's
    // 210 MB is 646 MB, so every lookup had to go to memory. At one point nine times the
    // measured peak it is three hundred and thirteen MB, which fits for the first time.
    //
    // Some room over is essential: too small and the table fills, and an order that cannot be
    // inserted is quietly dropped and the book is wrong from then on. Better to leave enough
    // than to cut it fine.
    const char* order_cap_env = std::getenv("ITCH_ORDER_CAP");
    // The environment variable is used if it is set, otherwise the default of 12<<20, which is
    // 12,582,912.
    // It is an environment variable rather than a command line switch because it is adjusted
    // very rarely - only to try whether a smaller table is quicker.
    const std::size_t orders_each =
        order_cap_env != nullptr ? std::strtoull(order_cap_env, nullptr, 10)
                                 : (12u << 20);
    // How large the price space has to be. budget_for adds up the narrowed row of reference
    // prices one at a time - each security gets eight times its own price up and down, so a
    // dear security takes more room.
    const std::size_t whole = book::PriceLevels::budget_for(prices);
    // With --symbols, ask for what was worked out plus a little over.
    // Without it this is the whole market path, which leaves half as much again over - ten
    // thousand and more names crowd in and the estimate is less accurate.
    const std::size_t words_each =
        opt.symbols != nullptr ? whole + (1u << 20)
                               : whole * 3 / 2 + (1u << 20);
    // Printed. This line has to be looked at once a run starts: it is the only evidence of how
    // large the tables of this round are, and their size decides directly whether they fit in
    // the third level cache.
    std::printf("%.2f GB of prices and %zu orders\n",
                words_each * 8.0 / 1e9, orders_each);

    // The flag that says it is time to finish. The main loop sets it once no packet has come
    // for a long time, and the acknowledgement thread exits on seeing it.
    // It is atomic because two threads touch it; it is a flag rather than a lock because
    // nothing on the hot path may ever be able to go to sleep.
    std::atomic<bool> done{false};
    // Every time the card is asked and says there is nothing, this goes up by one.
    // Only the main loop writes and reads it - it is atomic in case another thread ever wants
    // to look, and this function touches it once a poll, so the cost is nothing.
    // It is one of the tests that guard a measurement window: it has to have moved during the
    // quiet stretch before a window, because only then were we really idle once and only then
    // does that window count.
    std::atomic<std::uint64_t> drained{0};
    // Holds everything one core owns. The name is plural but only one ever goes in - the older
    // design that split the securities across several cores is gone and the container stayed.
    // A unique_ptr because that structure is hundreds of megabytes and would overflow the
    // stack.
    std::vector<std::unique_ptr<Shard>> shards;
    // Pinning the core has to come before asking the system for the book's memory, not after.
    //
    // Which half of the memory a huge page belongs to is decided by which core the first
    // thread to touch it was on, and this thread touches the whole block.
    // Unpinned it wanders, and the result is that the other half's huge pages are emptied out
    // - and the replay needs one of those itself, so it fails to start.
    if (opt.lock_memory) {
        // The order matters: the allocator has to be told not to give memory back before any
        // is asked for.
        // The other way round, these settings do nothing at all for what was already taken.
        mallopt(M_TRIM_THRESHOLD, -1);
        // The second call: forbid large allocations going through mmap.
        // glibc hands large allocations to mmap by default, and mmap memory goes straight back
        // to the system on free - the line above does not govern it, so both are needed.
        mallopt(M_MMAP_MAX, 0);
        // Pin every page the process has and will have into physical memory, never to be
        // swapped out.
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            // perror turns errno into plain words. The usual failure is a lack of permission.
            std::perror("mlockall");
            // Exit with 1. This path is unreachable at present, since lock_memory is always
            // false.
            return 1;
        }
        // Said out loud, to confirm this round really has it on.
        std::printf("memory held in place\n");
    }
    // The four window parameters, all read from the environment.
    // The replay and this program read the same environment variables, so each works out the
    // same answer on its own and neither has to tell the other.
    // The two sides have to be given the same values: with different ones the replay paces by
    // one set and this program judges windows by another, and the numbers mean nothing.
    const win::Params wp = win::params_from_env();
    // At most how many latency samples are kept. It decides how large each of the long row of
    // arrays below has to be.
    const char* cap_env = std::getenv("ITCH_SAMPLE_CAP");
    // Unset it is four million odd (4<<20). A sampled day sends fewer than two million orders,
    // which fits; a whole day replayed at the real speed is thirty times that, and then this
    // has to be raised through the environment.
    const std::size_t sample_cap =
        cap_env != nullptr ? std::strtoull(cap_env, nullptr, 10) : (4u << 20);
    // How many windows at the start are passed over rather than counted, from the environment.
    const char* skip_env = std::getenv("ITCH_SKIP_WINDOWS");
    // Unset, none are passed over. The first minutes after the open are the busiest of the
    // day, and passing some over is what lets the different stretches of the morning be
    // compared.
    const std::uint64_t skip = skip_env != nullptr ? std::strtoull(skip_env, nullptr, 10) : 0;
    // How many are counted after that, from the environment as well.
    const char* keep_env = std::getenv("ITCH_MAX_WINDOWS");
    // Unset, or set to 0, means all the rest.
    const std::uint64_t keep = keep_env != nullptr ? std::strtoull(keep_env, nullptr, 10) : 0;
    // Build the whole thing. This line takes hundreds of megabytes there and then - the order
    // table, the price space, and the initial capacity of all those statistics arrays.
    shards.push_back(
        std::make_unique<Shard>(orders_each, words_each, opt.threshold, wp));
    // Alternate paths on every poll, to compare them inside one round. Normally off.
    shards.back()->split_ab = opt.split_ab;
    // Which way the book is built: seven passes by default, one message at a time only with
    // --one-at-a-time.
    shards.back()->group = opt.group;
    // The two window selection settings above are handed over, for judging windows.
    shards.back()->skip = skip;
    // How many are wanted before it stops counting.
    shards.back()->keep = keep;
    // Everything below reserves room for the latency samples, taken once so that nothing is
    // asked for while running.
    //
    // Why an array must not grow by itself: it would grow exactly when an order has just gone
    // out and is being recorded, which is the middle of the hot path, and asking the system
    // for hundreds of megabytes takes several microseconds - microseconds that land inside the
    // window being measured, so what gets measured is how slow the allocator is rather than
    // how quick the system is.
    //
    // A sampled day sends fewer than two million orders and the default four million odd
    // fits; a whole day replayed at the real speed is thirty times that, which is why this
    // limit can be changed through the environment.
    shards.back()->raw.reserve(sample_cap);
    // assign followed by clear rather than reserve. The difference between them matters:
    //
    // reserve only claims the addresses and does not really take the memory - the operating
    // system's way is to hand a page over when it is first written, and that first write goes
    // into the kernel and back, one or two microseconds each. Hundreds of megabytes is a
    // hundred thousand pages, all of it on the hot path.
    //
    // assign writes a zero into every entry, forcing the operating system to hand the real
    // memory over now; clear only sets the length back to zero and leaves the pages and the
    // capacity.
    // This really did go wrong once: a neighbouring array used reserve, and one round took
    // over a thousand page faults on the hot path.
    shards.back()->raw_window.assign(sample_cap, 0);
    // The length goes to zero. The capacity and the pages already taken stay - which is the
    // whole point of the assign and clear pair: the memory now, the length from zero.
    shards.back()->raw_window.clear();
    // The ones below work the same way, assign then clear immediately.
    shards.back()->raw_rx.assign(sample_cap, 0);
    // raw_rx back to zero length, memory kept.
    shards.back()->raw_rx.clear();
    // Every pair below is the same two steps on a different row.
    // These rows are parallel: row i of each is about the same order, so together they can be
    // written out as one table.
    //
    // How many packets the poll that took this order found.
    shards.back()->raw_polln.assign(sample_cap, 0);
    // raw_polln back to zero length, memory kept.
    shards.back()->raw_polln.clear();
    // Which packet this order came from.
    shards.back()->raw_polli.assign(sample_cap, 0);
    // raw_polli back to zero length, memory kept.
    shards.back()->raw_polli.clear();
    // Which security.
    shards.back()->raw_sym.assign(sample_cap, 0);
    // raw_sym back to zero length, memory kept.
    shards.back()->raw_sym.clear();
    // When the poll returned.
    shards.back()->raw_poll.assign(sample_cap, 0);
    // raw_poll back to zero length, memory kept.
    shards.back()->raw_poll.clear();
    // When it entered the send path.
    shards.back()->raw_in.assign(sample_cap, 0);
    // raw_in back to zero length, memory kept.
    shards.back()->raw_in.clear();
    // When it left the send path.
    shards.back()->raw_out.assign(sample_cap, 0);
    // raw_out back to zero length, memory kept.
    shards.back()->raw_out.clear();
    // When the fields were written.
    shards.back()->raw_s1.assign(sample_cap, 0);
    // raw_s1 back to zero length, memory kept.
    shards.back()->raw_s1.clear();
    // When the TCP header was written.
    shards.back()->raw_s2.assign(sample_cap, 0);
    // raw_s2 back to zero length, memory kept.
    shards.back()->raw_s2.clear();
    // When the packet bodies were in hand.
    shards.back()->raw_body.assign(sample_cap, 0);
    // raw_body back to zero length, memory kept.
    shards.back()->raw_body.clear();
    // When the packets had all been parsed.
    shards.back()->raw_parse.assign(sample_cap, 0);
    // raw_parse back to zero length, memory kept.
    shards.back()->raw_parse.clear();
    // When the book had all been changed.
    shards.back()->raw_book.assign(sample_cap, 0);
    // raw_book back to zero length, memory kept.
    shards.back()->raw_book.clear();
    // When the doorbell had been pressed.
    shards.back()->raw_s3.assign(sample_cap, 0);
    // Back to zero. All twelve rows now hold real memory and none has to be asked for while
    // running.
    shards.back()->raw_s3.clear();
    // The two warm up arrays follow the same limit.
    // They used to be a fixed four million entries, and the reserve here touches every page -
    // with only a hundred and one names that would be sixty seven megabytes touched for
    // nothing.
    shards.back()->settle_at.reserve(sample_cap);
    // The second row of the pair: the latency at that moment.
    shards.back()->settle_ns.reserve(sample_cap);
    // The table of "do we trade this one" is only created when --symbols was given: sixty five
    // thousand entries of one byte each.
    // Without it the table stays empty, and empty means the whole market - see mine() in
    // take_packet.
    if (!wanted.empty()) shards.back()->traded.assign(65536, 0);
    // Several orders in one frame. This is always true now; the switch that turned it off is
    // gone.
    shards.back()->coalesce = opt.coalesce;
    // A mask of 0 means the whole day is replayed at the real speed and every message is
    // inside a window.
    // That way of running has no gaps between windows, so judging whether a window counts
    // follows different logic - see the every_unit branch in take_packet.
    shards.back()->every_unit = wp.mask == 0;

    // The room for the statistics is taken at once and every page of it touched now. The first
    // write to a new page on the hot path is a page fault of several microseconds, landing
    // exactly on what is being measured; touched before the run, it is gone.
    // The limit leaves room over the sixty one million polls measured; once full nothing more
    // is recorded, and recording less is better than running past the end.
    if (opt.stat != nullptr && !shards.empty()) {
        // Room for eighty three million rows. Twelve rows of eighty three million is a
        // gigabyte to touch, and touching every page takes several seconds before a run. So it
        // only happens with --stat.
        // A long round has to be checked against this figure for memory and disk beforehand -
        // over six and a half hours, polls.csv reaches gigabytes.
        constexpr std::size_t kStatCap = 80u << 20;
        // All twelve rows below use assign rather than reserve, for the reason above: assign
        // writes a zero into every entry and forces the operating system to hand the real
        // memory over now.
        // They are not cleared here - these rows are written by index, with stat_used
        // recording how far, rather than by push_back, so the length has to stay full.
        //
        // The market time of the first message of this batch.
        shards[0]->stat_when.assign(kStatCap, 0);
        // How many packets this poll found.
        shards[0]->stat_pkts.assign(kStatCap, 0);
        // How many messages there were in them altogether.
        shards[0]->stat_msgs.assign(kStatCap, 0);
        // How much time the arrival times of this batch spanned.
        shards[0]->stat_span.assign(kStatCap, 0);
        // How long it was between the previous poll and this one.
        shards[0]->stat_gap.assign(kStatCap, 0);
        // How long from having the packets to having finished with them.
        shards[0]->stat_proc.assign(kStatCap, 0);
        // How long from finishing to having sent.
        shards[0]->stat_send.assign(kStatCap, 0);
        // The four rows below are moments rather than differences - a difference can be worked
        // out afterwards, and the processor's clock and the card's have to come in pairs
        // before the drift between them can be fitted.
        //
        // The moment the event queue returned, on the processor's clock.
        shards[0]->stat_raw_poll.assign(kStatCap, 0);
        // The moment the packet bodies were in hand, on the processor's clock.
        shards[0]->stat_raw_body.assign(kStatCap, 0);
        // The moment work finished and before the send, on the processor's clock.
        shards[0]->stat_raw_done.assign(kStatCap, 0);
        // The hardware arrival time of the first packet of this batch, on the card's clock.
        // It is the only one measured by the card's ruler.
        shards[0]->stat_raw_nic.assign(kStatCap, 0);
        // How long between the last empty poll and this one finding packets - which is how
        // long a packet sat there without being asked for.
        shards[0]->stat_blind.assign(kStatCap, 0);
        // A line saying how much room it takes. A long round has to be checked against this
        // figure for memory and disk beforehand.
        std::printf("stat           room for %zu polls, %.1f GB, pre-touched\n",
                    kStatCap, kStatCap * 12.0 / (1u << 30));
    }
    // A line is printed when only some of the windows are counted. Without it, results looked
    // at later look as though the round covered the whole day.
    // Two environment variables control it: ITCH_SKIP_WINDOWS and ITCH_MAX_WINDOWS.
    // They are for iterating quickly - running only the few dozen windows after the open takes
    // a round from half an hour down to two or three minutes.
    if (skip != 0 || keep != 0) {
        // Printed as "counting windows first..last".
        // With keep at 0 it prints "end", meaning all the rest.
        std::printf("counting windows %llu..%s\n",
                    static_cast<unsigned long long>(skip + 1),
                    keep == 0 ? "end"
                              : std::to_string(skip + keep).c_str());
    }
    // Every shard has its own connection and its own queue on the card, and nothing on the
    // send path is shared.
    // Whether --order-ip was given decides whether this round sends orders at all.
    // A round that does not send only builds the book and works out signals, which measures
    // the processing on its own.
    const bool trading = opt.order_ip != nullptr;
    // Everything below - opening the send interface and shaking hands - is only needed to send
    // orders, and a round that does not send skips the whole section.
    if (trading) {
        // Open the interface used only for sending, with a send buffer of sixty four slots.
        if (!open_order_path(shards[0].get(), opt.intf)) {
            // It would not open. This round was meant to send orders, and unable to, it is
            // pointless, so it exits.
            std::fprintf(stderr, "could not open the order path\n");
            // Exit with 1 - without the send interface this round is pointless.
            return 1;
        }
        // This pair of braces only fences the handshake off and does nothing else.
        {
            // The socket the kernel's stack opened above is exactly what the filter takes the
            // packets away from; from here the connection is ours. That way the two paths
            // differ by one switch.
            // The handshake also happens on the acknowledgement queue: the SYN-ACK coming back
            // carries the order connection's five values, so the card's filter puts it in
            // ack_vi and it never appears on the market data one.
            // This step runs before the acknowledgement thread starts, so polling here does
            // not compete with it.
            if (!shake_hands(shards[0].get(), ack_vi, ack_frames, ack_prefix,
                             kAckRingSlots, &ack_post, local_ip,
                             static_cast<std::uint16_t>(51000 + 0), peer_ip,
                             static_cast<std::uint16_t>(opt.order_port + 0),
                             opt.intf)) {
                // The handshake failed. The three usual causes are that the exchange side is
                // not running, that the system's neighbour table has no card address for the
                // far end (a ping after a reboot puts it there), or that the TCP filter above
                // is wrong and the reply never arrives.
                std::fprintf(stderr, "could not open our own connection\n");
                // Exit with 1 - without a handshake not one order can go out.
                return 1;
            }
            // The far end's starting sequence number is printed. It differs every round, since
            // the far end seeds from the low bits of its clock too - so this line also proves
            // the handshake really happened again rather than something left over from the
            // previous round.
            std::printf("own connection opened, they start at %u\n",
                        shards[0]->conn.rcv_nxt());
            // A line saying what this round's window limit is and whether scaling was really
            // agreed.
            // When it is not, the shift is 0 and the limit stays the 65,535 of the protocol's
            // sixteen bit field.
            std::printf("window scale   they shift by %u, so at most %u bytes"
                        " may be unacknowledged\n",
                        shards[0]->conn.peer_shift(),
                        65535u << shards[0]->conn.peer_shift());
        }
    }
    // Whether this round records the figures of every poll. Read into a local so that the hot
    // path has a boolean in a register rather than reaching through options every time.
    const bool stat_on = opt.stat != nullptr;
    // Whether the breakdown into segments is measured. It adds two fenced clock reads to every
    // poll, and over sixty million polls that is more than a second, so the baseline does not
    // carry that cost: it goes on only when those segments are actually wanted.
    // It must not depend on whether --out was given - that is passed every round, which would
    // mean always on.
    const bool timing = stat_on || opt.segments;
    // Whether it is on this round is printed at the start. A whole day has been wasted once by
    // forgetting the state of a switch.
    std::printf("segments       %s\n",
                timing ? "on, six segments timed per poll"
                       : "off, no per-poll clock reads");
    // How long without an acknowledgement before a frame is taken as lost and sent again. One
    // millisecond here.
    //
    // Why a fixed number is safe, rather than measuring the round trip as real TCP does:
    // this is a direct cable to the machine next door and the real round trip is a few
    // microseconds.
    // A millisecond is five hundred times that - the far end being a little slow can never be
    // mistaken for a loss, while a frame that really was lost is replaced within a
    // millisecond.
    //
    // Converted to the processor counter's units in advance, so the hot path does an integer
    // subtraction and no conversion.
    const std::uint64_t rto_ticks =
        static_cast<std::uint64_t>(1e6 * tsc::ticks_per_ns());

    // The thread that reads acknowledgements. It does one thing: poll the acknowledgement
    // queue and hand what it reads to the connection, so that "how much they have received"
    // keeps moving. It sends no orders, touches no book and works out no signals, so the hot
    // send path is not one nanosecond slower - which is exactly why it was pulled out.
    //
    // Why it has to be another thread rather than one more poll inside the main loop: while
    // the main loop is working through a batch of eight hundred odd messages, it would not be
    // polling the second queue either. Splitting the queues without splitting the threads
    // splits nothing.
    std::thread ack_thread;
    // A round that does not send orders does not need it - with no orders there are no
    // acknowledgements.
    if (trading) {
        // Pinned to the core next to the working thread. Those two cores share the same third
        // level cache, and the two threads read and write the same few values of one
        // connection back and forth; on two cores that share a cache those values travel
        // inside the chip rather than going round through memory.
        const int ack_cpu = opt.cpu_base < 0 ? -1 : opt.cpu_base + 1;
        // Start the thread. The [&, ack_cpu] is how it captures: everything else by reference,
        // since it outlives this thread, and only the core number by value - it is a local
        // outside the loop and by value is simplest.
        ack_thread = std::thread([&, ack_cpu] {
            // The new thread pins itself first. Every thread has to do this itself - pinning
            // applies to a thread, not to a process.
            if (ack_cpu >= 0) pin(ack_cpu);
            // The entry used to post back, as on the market data queue: what goes back is
            // never the entry just read.
            // It carries on from where the handshake stopped, so it does not collide with the
            // entries the handshake already posted.
            std::size_t give = ack_post & (kAckRingSlots - 1);
            // The main loop sets this when it finishes, and this thread exits on seeing it.
            while (!done.load(std::memory_order_acquire)) {
                // At most sixteen at a time. Acknowledgements come a few thousand a second, so
                // sixteen is ample.
                ef_event evs[16];
                // Go and see whether the acknowledgement queue has anything. This loop spins
                // extremely fast - nearly always it takes none and is simply spinning. That is
                // what this core is for.
                const int n = ef_eventq_poll(ack_vi.get(), evs, 16);
                // Every event taken is looked at.
                for (int i = 0; i < n; ++i) {
                    // Only "a packet arrived". This interface only receives, so in theory no
                    // other type can appear, but the line costs nothing.
                    if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
                    // The number we gave this buffer when it was posted, handed back unchanged.
                    const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
                    // How many bytes this packet is, less the prefix - the card put a short
                    // piece of its own in front, and that is not part of the packet.
                    const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - ack_prefix;
                    // What the connection is given is the first byte of the frame rather than
                    // somewhere in the middle: the offsets used to read the acknowledgement
                    // number are all counted from the start of the frame.
                    take_ack(shards[0].get(), ack_frames.at(slot) + ack_prefix, len);
                    // Having read this entry, another goes back to the card, so the ring stays
                    // full.
                    if (ef_vi_receive_init(ack_vi.get(), ack_frames.dma(give), give) >= 0) {
                        // Only a successful post moves on; a failure uses this entry again next
                        // time.
                        // That & (kAckRingSlots - 1) is a modulo by 512 - since 512 is a power
                        // of two, a bitwise and is far quicker than a division.
                        give = (give + 1) & (kAckRingSlots - 1);
                    }
                }
                // The doorbell is pressed only if something really arrived. Whatever was posted
                // above is told to the card in one go, rather than a press per entry - each
                // press crosses the bus.
                if (n > 0) ef_vi_receive_push(ack_vi.get());
            }
        // This line closes two things at once: the brace closes the function the thread runs,
        // and the parenthesis closes the construction of the std::thread. The thread is already
        // running from here on.
        });
        // A line saying which queue acknowledgements use and which core polls it.
        std::printf("ack path       its own queue on %s, polled by a thread on cpu %d\n",
                    opt.intf, ack_cpu);
    }

    // Before the run, a copy of the card's own counters is taken.
    // Another copy is taken at the end, and comparing the two says whether the card dropped
    // any packets this round - a round that dropped packets has incomplete samples and is
    // worthless entirely.
    nic::Drops before;
    // Whether they could be read at all. If not, for instance for want of permission, the
    // comparison at the end is skipped.
    const bool have_counters = nic::read_drops(opt.intf, &before);

    // How many ticks the clock of this machine takes per nanosecond, which several places
    // below use to convert between nanoseconds and ticks.
    const double tps = tsc::ticks_per_ns();
    // "How long without anything arriving counts as finished", in ticks.
    // Converted in advance so that the test on the hot path is an integer subtraction rather
    // than a floating point multiplication.
    const std::uint64_t idle_ticks = static_cast<std::uint64_t>(opt.idle_ms * 1e6 * tps);
    // When something last arrived, and when the first packet arrived, where 0 means it has not
    // started.
    // The first decides when to finish, the second gives how long the round ran altogether.
    std::uint64_t last_seen = tsc::now(), start = 0;
    // How many packets arrived altogether, and how many of them were dropped for being too
    // short or not ours.
    std::uint64_t packets = 0, discards = 0;
    // Goes up on every poll, and is used to keep one record every 1024 polls whether that poll
    // was deep or not.
    // It is these ordinary records that say what a poll looks like when we are not behind, and
    // the deep ones can only be read against them.
    std::uint64_t poll_seq = 0;
    ef_event evs[kMaxPollEvents];

    // This is the main loop of the whole trader, and running a day is running this.
    //
    // One turn does four things, always in this order:
    //   1 ask the card whether there are new packets   (ef_eventq_poll)
    //   2 if there are, hand each to take_packet to build the book, work out signals and write
    //     orders
    //   3 send whatever orders this batch produced     (flush_orders)
    //   4 give the receive buffers used back to the card and press the doorbell once
    //
    // It never looks inside a packet itself. It only takes them out and hands them on; the
    // content is all in take_packet.
    for (;;) {
        // This is the hottest line in the whole program - it runs tens of millions of times in
        // a day.
        //
        // ef_eventq_poll is "go and see whether the card has anything to say".
        // Three arguments: which interface, an array to put the events in, and how many that
        // array holds.
        // It returns how many were taken, and zero means there was nothing.
        //
        // It differs from a socket's recv in three fundamental ways, and those three are the
        // whole benefit of bypassing the kernel:
        //   1 it does not enter the kernel. It reads a queue the card writes straight into our
        //     memory, and the whole call is a few dozen instructions with no system call and no
        //     context switch.
        //   2 it does not block. With nothing there it returns 0 at once, so we go on asking
        //     forever - the cost is a core held at 100%, and what it buys is no wake up delay
        //     at all.
        //   3 it does not copy. What it gives is an event, not the data; the packet is still
        //     lying in the memory the card wrote it into, and we take the number, look up the
        //     address and read it where it lies.
        //
        // At most kMaxPollEvents, 256, at a time. Not taking them all is fine and the next turn
        // takes the rest.
        // The limit is not arbitrary: in a market burst a poll really does take dozens.
        // Changing it means checking who else is sized by that number - raising it from 64 to
        // 256 once missed one, and a deep poll wrote past the end of an array and killed the
        // trader on the spot.
        const int n = ef_eventq_poll(vi.get(), evs, kMaxPollEvents);
        // Nothing was taken. It looks like a wasted turn, and in fact ninety nine percent of
        // the polls in a day come here - and it is not empty: the three pieces of upkeep below
        // all happen here.
        if (n == 0) {
            // This moment is recorded. Next time packets really are taken, subtracting it
            // gives when we last asked.
            if (stat_on) shards[0]->last_empty = tsc::now();
            // e0 to e3 below cut this branch into three parts, to see how long each takes at
            // worst.
            // It is ninety nine percent of the time, so a stall here makes every packet after
            // it late.
            const std::uint64_t e0 = stat_on ? tsc::now() : 0;
            // One more empty turn.
            // Its only use is guarding the measurement windows: when a window starts, whether
            // this number has moved is checked, and if it has not, we were never idle during
            // the whole quiet stretch and that window does not count.
            drained.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t e1 = stat_on ? tsc::now() : 0;
            // The card's queue is empty, and this line is saying that we have caught up.
            // It is not state recorded in passing - whether a measurement window counts rests
            // on it: during the quiet stretch before a window, the queue must really have been
            // seen empty once, or what that window measures is a backlog left over from before
            // rather than a latency.
            shards[0]->caught_up = true;
            if (trading) {
                // Collect the card's send completions: which frames really reached the wire
                // and when.
                // The latency of an order is worked out and recorded here - at the moment of
                // sending it is not yet known.
                reap(shards[0].get());
                const std::uint64_t e2 = stat_on ? tsc::now() : 0;
                // Idle now, which is the moment to look back at whether any order sent has
                // gone unanswered.
                // Putting it in this branch is deliberate - with packets to handle there is not
                // a second to spare, while "nothing arrived" is a moment with nothing else to
                // do.
                resend_stale(shards[0].get(), rto_ticks);
                if (stat_on) {
                    const std::uint64_t e3 = tsc::now();
                    Shard* sh = shards[0].get();
                    // The three parts are: the counter above, collecting the send completions,
                    // and looking for orders that went unanswered.
                    // Two things are recorded below: how long each part took at worst, and how
                    // often it went past ten microseconds.
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
            // Packets came and then twenty seconds of quiet - the replay has finished and this
            // round ends normally.
            // There is no "I have finished" message on the market data path, so it can only be
            // inferred from how long the quiet has lasted.
            if (packets != 0 && tsc::now() - last_seen > idle_ticks) break;
            // Not one packet from beginning to end, after three hundred seconds - this is not
            // an ending, it is a run that never began.
            // Usually the far end is not running, the filter is wrong, or the cable is in the
            // wrong port. So it reports and exits.
            //
            // Why this one waits fifteen times as long: before it starts, the replay reads
            // tens of gigabytes of file and builds over a million packets in advance, and we
            // have been spinning here throughout.
            // With less patience, every round would declare failure while the far end was
            // still getting ready.
            if (packets == 0 && tsc::now() - last_seen > idle_ticks * 15) {
                std::fprintf(stderr, "nothing arrived\n");
                done.store(true, std::memory_order_release);
                // The acknowledgement thread is waiting on that flag too, and without joining
                // it the process does not exit cleanly.
                if (ack_thread.joinable()) ack_thread.join();
                return 1;
            }
            continue;
        }
        // From here on is the branch where something really was taken. The moment is recorded
        // first, since both tests for finishing above depend on it.
        last_seen = tsc::now();
        // How many were taken this time says how far behind we are - when keeping up it is one
        // or two.
        // Recording it costs nothing: n is in a register, and this is once per poll rather than
        // once per packet.
        // There are only 64 buckets. Anything above 64 goes into the last one, so that bucket
        // reads as "64 or more" rather than exactly 64.
        ++shards[0]->depth[n < 65 ? n : 64];
        // This little loop reads nothing at all; it only has a word with memory first.
        //
        // An event only says which entry a packet is in, and the packet body is still lying in
        // memory untouched.
        // And on this processor the card cannot write a packet straight into cache (Intel has
        // that path, AMD does not), so the first read of every packet really goes to memory,
        // about two hundred cycles.
        //
        // One at a time, those trips are end to end: ten packets means waiting ten times.
        // Telling the memory controller all the addresses of the batch first lets it fetch them
        // together - ten waits overlap into one.
        // And the more packets were taken the more can overlap, which is exactly when we are
        // furthest behind.
        //
        // Two words per packet, at 0 and at 64 bytes, because a packet in a window is about
        // ninety nine bytes and two cache lines usually cover the whole of it.
        // Point 1: the event queue has just returned. At this moment all we hold is up to 256
        // entry numbers, and the bodies are still lying in memory untouched. What sits between
        // this and point 2 is getting the bodies in hand.
        // Timing costs something: two fences and a clock read, about twenty nanoseconds a poll,
        // which over sixty million polls is more than a second. A baseline run does not measure
        // these two parts, so it goes by a switch: the cost is only paid when samples are being
        // written or --stat is on, and otherwise this is one comparison.
        const std::uint64_t g1 = timing ? fenced_now() : 0;
        for (int i = 0; i < n; ++i) {
            // Events that are not packets are skipped - drops and truncations are dealt with in
            // the loop below.
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) continue;
            // The entry number gives the address of the packet in our memory. This step does
            // not touch the body; it only works out an address.
            const std::uint8_t* b = frames.at(EF_EVENT_RX_RQ_ID(evs[i]));
            // Three arguments: which address, read or write (0 is read), and how long it should
            // be kept (3 is keep it if possible).
            // It returns nothing and cannot fail - even a bad address is simply ignored.
            __builtin_prefetch(b, 0, 3);
            __builtin_prefetch(b + 64, 0, 3);
        }
        // Point 2: fetching the packets is done.
        // This part measures as almost zero, and that does not mean fetching is free - the loop
        // above only has a word with memory and moves on without waiting for the data.
        // The real wait happens later, at the first read of a packet body, and is recorded in
        // the part at point 3.
        const std::uint64_t g2 = timing ? fenced_now() : 0;
        shards[0]->cur_body = g2;
        // These two are cleared every turn and filled in as the packets are parsed below.
        // poll_msgs  how many ITCH messages this batch holds altogether. It is not the same as
        //            n: n is packets, and a packet may hold several messages.
        // poll_when  the market time of the first message of this batch.
        shards[0]->poll_msgs = 0; shards[0]->poll_when = 0;
        // The number of this poll, counting up from beginning to end.
        ++poll_seq;
        // Whether this poll falls inside a measurement window. Several places below look at
        // this.
        //
        // Why only windows are recorded: outside them the rest of the day's messages are pushed
        // through at a speed of our own choosing, which produces not one latency sample while
        // taking dozens of times as many polls. Recording it would be useless and take up room.
        //
        // This uses which stretch the last message fell in, so a poll sitting exactly on a
        // boundary may be counted in the neighbouring stretch - one poll out, which is
        // acceptable.
        const bool in_window = shards[0]->was == win::Phase::kWindow;
        // Use g1 from above rather than reading the clock again here. g1 is the moment the
        // event queue returned, while this line is already past the fetching; reading here
        // would make the fetching part come out negative.
        const std::uint64_t poll_tsc_always = in_window ? g1 : 0;
        // That one only holds a value inside a window and is 0 outside.
        // With --stat on, every poll needs a moment, so one is read here for the polls outside.
        // Why they are wanted too: the point is to see how the stalls are spread through a whole
        // day, not only through the windows.
        const std::uint64_t stat_poll_tsc =
            (stat_on && poll_tsc_always == 0) ? tsc::now() : poll_tsc_always;
        // The three below are filled in while the batch is parsed, in different units and on
        // different clocks:
        // first_body  when a packet body was really read for the first time, on the processor's
        //             clock
        std::uint64_t first_body = 0;
        // first_rx    the arrival time of the first packet of this batch, on the card's clock,
        //             which is not the same ruler as the processor's
        std::uint64_t first_rx = 0;
        // The arrival time of the last packet of this batch. The span from first_rx to it is
        // how long the card says these packets took to arrive.
        // Set against how long we went without polling, it tells a stall of ours apart from the
        // card handing them over late.
        std::uint64_t last_rx = 0;
        // The two paths alternate, one per poll. They alternate whether or not this poll is
        // timed, because otherwise the two paths would not see the same market data and the
        // difference between them would carry "which one happened to catch the open".
        if (shards[0]->split_ab) {
            shards[0]->collect = (shards[0]->polls_done++ & 1) != 0;
        }
        // Only polls that took more than one event are timed. Nine in ten take exactly one, and
        // reading the clock for those would cost more than the work they do.
        // n is known the moment the poll returns, so this test does not have to wait for the
        // parsing.
        const bool timed = shards[0]->split_ab && n >= 2;
        // How many messages had been applied before the timing starts; subtracting gives how
        // many this poll applied.
        const std::uint64_t applied_before = timed ? shards[0]->applied : 0;
        // The starting point. It has to be before the parsing, because writing into the arrays
        // happens during the parsing.
        const std::uint64_t ab_t0 = timed ? tsc::now() : 0;
        for (int i = 0; i < n; ++i) {
            // These three are stored with every order this packet leads to.
            // With them, a very slow sample can be read as "that poll took sixty packets and
            // this was the sixtieth" rather than as a large number with no reason attached.
            shards[0]->poll_n = static_cast<std::uint16_t>(n);
            shards[0]->poll_i = static_cast<std::uint16_t>(i);
            shards[0]->poll_at = poll_tsc_always;
            // An ef_event is one thing the card has to say to us.
            // It is a union and what it holds depends entirely on its type, so the first thing
            // is always to ask the type.
            // Reading another field without checking reads the bytes of a different kind of
            // event, and nothing reports it.
            //
            // The types that can appear on this interface:
            //   EF_EVENT_TYPE_RX               a packet arrived - the one we want
            //   EF_EVENT_TYPE_RX_DISCARD       the card received it and decided to throw it
            //                                  away (a bad checksum, an invalid length)
            //   EF_EVENT_TYPE_RX_NO_DESC_TRUNC received but with no free buffer to put it in,
            //                                  so it had to be truncated - which means we could
            //                                  not keep up
            //   EF_EVENT_TYPE_TX / TX_WITH_TIMESTAMP  completions for frames sent
            //                                  (this interface does not send, so they never
            //                                  appear)
            if (EF_EVENT_TYPE(evs[i]) != EF_EVENT_TYPE_RX) {
                // The last two have to be counted. A non zero means this round really did lose
                // packets, and then the percentiles measured are false.
                if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_NO_DESC_TRUNC ||
                    EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX_DISCARD) {
                    ++discards;
                }
                continue;
            }
            // This number is the one we gave ourselves when posting the buffer with
            // ef_vi_receive_init.
            // The card knows nothing about our pointers and only hands the number back.
            // So where a packet is, is something we look up by number rather than something the
            // card tells us.
            const std::size_t slot = EF_EVENT_RX_RQ_ID(evs[i]);
            // How many bytes this packet is.
            // It includes the prefix the card wrote itself, which has to come off to give the
            // real length of the Ethernet frame.
            // Forgetting to subtract it makes cutting the messages by length produce one extra
            // piece of rubbish.
            const std::size_t len = EF_EVENT_RX_BYTES(evs[i]) - prefix;
            // The body is in the memory the card wrote it into, found by entry number and read
            // where it lies.
            std::uint8_t* buf = frames.at(slot);
            // There used to be a test here that picked out the acknowledgements: read the byte
            // of the IP header that says what the layer above is, and treat TCP as an
            // acknowledgement. Acknowledgements now have a queue of their own and only market
            // data is left here, so that test and its read and comparison per packet are gone.
            if (packets == 0) start = tsc::now();
            ++packets;

            ef_precisetime ts{};
            // This line takes out the arrival time the card stamped. It is where the whole wire
            // to wire measurement starts.
            //
            // The moment is not in the packet but in the short prefix the card wrote in front
            // of it (see prefix).
            // Rather than parsing that prefix ourselves, the interface and the start of the
            // buffer are handed over and it is taken out in this card's format - the format
            // differs between models.
            //
            // Three arguments: which interface, where this packet starts in our memory, and
            // where to put the result.
            // The second has to be the start including the prefix, not the position past it.
            //
            // ef_precisetime is the result type: seconds, nanoseconds, and an indication of
            // accuracy.
            // Only the first two are used.
            ef_vi_receive_get_precise_timestamp(vi.get(), buf, &ts);
            // Combined into a number of nanoseconds.
            // This is the card's own clock, and the processor's counter is a different crystal.
            // Measured, the two differ by more than three thousand parts per million, which is
            // one and a half milliseconds of drift in four hundred and fifty seconds.
            // So subtracting one from the other directly means nothing; they have to be lined
            // up first by taking the smallest value in a sliding window.
            const std::uint64_t at =
                static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
            // The first packet of this batch is the one that waited on us longest.
            // so how late we were is worked out from it rather than from the last one.
            if (first_rx == 0) first_rx = at;
            // Overwritten by every packet, so after the loop it holds the arrival time of the
            // last packet of the batch.
            last_rx = at;
            // The first byte of this packet is read deliberately, and the moment recorded right
            // after.
            // That read is the real wait on memory - the prefetch above only had a word and did
            // not wait.
            //
            // It looks wasteful and costs almost nothing: take_packet is about to read it
            // anyway, and once read that cache line is in cache and its read is free.
            // volatile keeps the compiler from optimising this read away.
            if (stat_on && first_body == 0) {
                volatile const std::uint8_t* probe = buf + prefix;
                (void)*probe;
                first_body = tsc::now();
            }
            take_packet(shards[0].get(), buf + prefix,
                        static_cast<std::uint32_t>(len), at, &reference,
                        &wanted, trading, &drained);
            // Which entry goes back to the card: straight through from beginning to end, then
            // round again.
            // What really decides how far behind we may fall is not the depth of the card's
            // queue but this - an entry waits for all eight million others to be used before it
            // is written a second time.
            const std::size_t give = next_post & (cfg::kRxRingSlots - 1);
            ++next_post;
            // The entry is posted back into the receive ring, telling the card it may write a
            // packet there again.
            // init without push: push is the doorbell and crosses the bus, so it is pressed
            // once after the whole batch, which is where this loop ends.
            // A failure is counted. It can only fail when the ring is full, and that means we
            // are already unable to keep up.
            if (ef_vi_receive_init(vi.get(), frames.dma(give), give) < 0) ++discards;
        }
        // Point 3: the packets of this batch have all been parsed and not one entry of the book
        // has changed.
        // What sits between this and point 2 is the parsing.
        shards[0]->cur_parse = timing ? fenced_now() : 0;
        apply_hits(shards[0].get(), trading);
        // The end point. It has to be after apply_hits - on the two pass path, the second pass
        // is here.
        // Working out signals is left out: it is identical on both paths and including it would
        // only dilute the difference.
        if (timed) {
            // How many entries of the book this poll really changed. Measured over a first
            // round, a poll averages only 0.22 of them, which is to say the vast majority of
            // timed polls touch the book not at all - averaging those in would measure the cost
            // of fetching and parsing, and those two are identical on both paths and would only
            // dilute the difference being looked for. So a poll that touched nothing is not
            // recorded.
            // The clock is still read, because the starting point has to be before the parsing
            // and at that moment how many will be touched is unknown; and both paths read the
            // clock equally often, so that cost cancels out.
            const std::uint64_t touched = shards[0]->applied - applied_before;
            if (touched != 0) {
                const std::size_t w = shards[0]->collect ? 1 : 0;
                shards[0]->ab_ticks[w] += tsc::now() - ab_t0;
                ++shards[0]->ab_polls[w];
                shards[0]->ab_msgs[w] += touched;
            }
        }
        // The signal is worked out once after the whole batch rather than once per message.
        // One poll may change the same security several times, and only the book as the last of
        // them left it counts.
        // Point 4: the book is changed and no signal has been worked out yet.
        // What sits between this and point 3 is the seven passes of apply_hits, which is
        // building the book.
        shards[0]->cur_book = timing ? fenced_now() : 0;
        settle_dirty(shards[0].get(), trading);
        // The second of three moments: by here all the work of this batch is done - the packets
        // parsed, the book built, the signals worked out, and the orders to send written as
        // bytes.
        // All that is left is handing the last part frame of them to the card.
        // settle_dirty has to be above this line, because working out signals is part of the
        // processing; below it, the processing time measured would leave it out.
        const std::uint64_t done_tsc = tsc::now();
        // The orders this batch gathered that do not fill a frame go out now.
        // They must not wait for the next poll - once the market quietens the next poll may be
        // a long time away and those orders would sit in hand. And a quiet market is not worth
        // gathering for anyway.
        if (trading) flush_orders(shards[0].get());
        // The third moment: the orders are with the card and the work of this turn ends here.
        const std::uint64_t sent_tsc = tsc::now();
        // One statistics record for this poll. It is here because only by now have the packets
        // and the messages all been counted.
        // Without --stat these arrays are empty and size() is 0, so in an ordinary run this
        // line is one comparison and the whole section is skipped.
        if (shards[0]->stat_used < shards[0]->stat_when.size()) {
            const std::size_t k = shards[0]->stat_used++;
            shards[0]->stat_when[k] = shards[0]->poll_when;
            shards[0]->stat_pkts[k] = static_cast<std::uint16_t>(n);
            shards[0]->stat_msgs[k] = static_cast<std::uint16_t>(
                shards[0]->poll_msgs > 65535 ? 65535 : shards[0]->poll_msgs);
            // The arrival span: how long there was between the first and the last packet of
            // this batch, on the card's clock.
            const std::uint64_t span = last_rx > first_rx ? last_rx - first_rx : 0;
            shards[0]->stat_span[k] = static_cast<std::uint32_t>(
                span > 0xffffffffull ? 0xffffffffu : span);
            // How long we went without polling, on the processor's clock, in nanoseconds.
            const std::uint64_t gap = stat_poll_tsc > shards[0]->last_poll_seen
                ? static_cast<std::uint64_t>(
                      (stat_poll_tsc - shards[0]->last_poll_seen) / tps)
                : 0;
            shards[0]->stat_gap[k] = static_cast<std::uint32_t>(
                gap > 0xffffffffull ? 0xffffffffu : gap);
            // The processing part: from having the packets to having finished with them.
            const std::uint64_t proc = done_tsc > stat_poll_tsc
                ? static_cast<std::uint64_t>((done_tsc - stat_poll_tsc) / tps) : 0;
            shards[0]->stat_proc[k] = static_cast<std::uint32_t>(
                proc > 0xffffffffull ? 0xffffffffu : proc);
            // The sending part: from finishing to having sent.
            const std::uint64_t snd = sent_tsc > done_tsc
                ? static_cast<std::uint64_t>((sent_tsc - done_tsc) / tps) : 0;
            shards[0]->stat_send[k] = static_cast<std::uint32_t>(
                snd > 0xffffffffull ? 0xffffffffu : snd);
            // The four raw moments are stored as they are, neither subtracted nor converted.
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
        // The gap of the next poll is measured from the moment this one finished sending rather
        // than from when it took the packets - that way the three parts join end to end and
        // add up to exactly the whole time between two polls, with nothing counted twice and
        // nothing missed.
        shards[0]->last_poll_seen = sent_tsc;
        // The batch is done, so the doorbell is pressed once and all the entries just posted
        // are told to the card together.
        // Here rather than once per entry, because pressing the doorbell crosses the bus.
        // On a poll that took 64 packets this saves 63 of them.
        ef_vi_receive_push(vi.get());
    }
    // How many seconds of wall clock this round ran, for printing at the end.
    const double wall = (last_seen - start) / tps / 1e9;

    // Tell the working threads that the replay has finished.
    // release rather than a relaxed write.
    // It guarantees that everything published before it is visible before this flag is seen.
    // Without it a thread could see "finished" while missing the last few messages.
    done.store(true, std::memory_order_release);
    // The finishing section polls the acknowledgement queue itself, so the acknowledgement
    // thread is joined first - two threads must not watch one queue, which allows only one
    // consumer.
    if (ack_thread.joinable()) ack_thread.join();
    // Close the connection we opened ourselves.
    //
    // What happens without closing it: the far end waits forever on a stream that never ends,
    // and so never prints how many orders it received.
    // And that number is the one check that can catch a frame laid out wrongly - not closing is
    // the same as not checking.
    for (auto& sh : shards) {
        // Anything not yet acknowledged is sent again first, so the stream is complete before
        // it closes.
        // For at most two more seconds.
        // The limit is essential: if the far end has gone, without it this spins forever.
        const std::uint64_t stop = tsc::now() +
            static_cast<std::uint64_t>(2e9 * tsc::ticks_per_ns());
        // Wait until every frame sent has been acknowledged, or the two seconds are up.
        while (sh->acked_frames < sh->frames && tsc::now() < stop) {
            ef_event evs[8];
            // This polls the acknowledgement queue rather than the market data one -
            // acknowledgements no longer arrive there.
            const int got = ef_eventq_poll(ack_vi.get(), evs, 8);
            for (int k = 0; k < got; ++k) {
                // The queue also carries send completions and the like, so only arrivals are
                // taken.
                if (EF_EVENT_TYPE(evs[k]) != EF_EVENT_TYPE_RX) continue;
                // The number we gave each entry when posting the buffers, handed back unchanged.
                // The card knows nothing about our addresses and only returns the number.
                const std::size_t sl = EF_EVENT_RX_RQ_ID(evs[k]);
                // How many bytes, less the short piece the card wrote in front of the frame.
                const std::size_t ln = EF_EVENT_RX_BYTES(evs[k]) - ack_prefix;
                // Handle this acknowledgement, moving "how much they have received" along.
                take_ack(sh.get(), ack_frames.at(sl) + ack_prefix, ln);
                // A new entry goes back in its place.
                // The next entry rather than the one just read, for the same reason as on the
                // market data side: a buffer is overwritten again only after a long time.
                const std::size_t give = ack_post & (kAckRingSlots - 1);
                ++ack_post;
                (void)ef_vi_receive_init(ack_vi.get(), ack_frames.dma(give), give);
            }
            // Press the doorbell once so the card sees the entries just posted.
            // Once per batch rather than once per entry.
            if (got > 0) ef_vi_receive_push(ack_vi.get());
            // Frames too long unacknowledged are sent again. The finishing section has to do it
            // too, or one lost frame stalls until the timeout.
            resend_stale(sh.get(), rto_ticks);
        }
        // A frame carrying only a FIN goes out, telling the far end we have finished.
        std::uint8_t* slot = sh->txbuf.at(sh->next_slot);
        // No data at all, only the FIN and ACK flags.
        const std::size_t n = sh->conn.send(slot, nullptr, 0,
                                            mintcp::kFin | mintcp::kAck);
        // Handed to the card. It is not waited for - the process is about to exit and whether
        // this frame gets out affects no number.
        (void)ef_vi_transmit(sh->tx.get(), sh->txbuf.dma(sh->next_slot),
                             static_cast<int>(n),
                             static_cast<ef_request_id>(sh->next_slot));
        std::printf("own connection closed, %" PRIu64 " messages resent, "
                    "%" PRIu64 " resets or closes from the far side\n",
                    sh->resends, sh->peer_gone);
    }

    // The card's own drop counters are read again and compared with the copy from the start.
    nic::Drops after;
    // Three things have to hold for it to be clean: they could be read at the start, they can
    // be read now, and not one of the four counters has moved.
    // Without the first two, "cannot be read" would be taken as "nothing was dropped".
    const bool clean = have_counters && nic::read_drops(opt.intf, &after) && before == after;

    // What follows gathers the counters of the shards together so they can be printed as one
    // report.
    // Several of them are the conditions for accepting a round at all, and each is marked as
    // such where it is printed.
    //
    // Messages, how many really changed the book, and how many orders went out on each side.
    std::uint64_t messages = 0, applied = 0, buys = 0, sells = 0;
    // Of those, the ones inside a measurement window. It is the denominator of the order rate.
    std::uint64_t applied_window = 0;
    // Four numbers that should never happen, plus how many orders are resting now:
    //   gaps       a hole in the sequence numbers
    //   duplicates duplicate packets - the same content on both paths, which is not a fault
    //   lapped     lapped, meaning messages really were lost
    //   orphan     a message mentioning an order id we do not hold
    //   live       orders resting
    std::uint64_t gaps = 0, duplicates = 0, lapped = 0, orphan = 0, live = 0;
    // How often the order table filled, and how many securities did and did not get a price
    // space.
    std::uint64_t full = 0, bound = 0, unbound = 0;
    // How many orders went out, how many were turned away, how many of those sent were
    // recorded as samples, and how many were stopped for want of a slot.
    std::uint64_t sent = 0, refused = 0, stamped = 0, no_slot = 0;
    // How many were stopped by the far end's window, and the smallest and largest window it
    // reported when that happened.
    std::uint64_t wnd_block = 0;
    std::uint32_t wnd_min = 0xffffffffu, wnd_max = 0;
    // How many times the send path was warmed by a send that goes nowhere.
    std::uint64_t warmed = 0;
    // How many messages there were inside windows altogether, and how many orders really went
    // out in the 1:1 stretches.
    std::uint64_t window_messages = 0, paced_orders = 0;
    // How many windows there were, how many were thrown away, and how many samples went with
    // them.
    // A window is thrown away because we had not caught up before it began, and its samples
    // measure a backlog rather than a latency.
    std::uint64_t windows = 0, windows_dropped = 0, dropped_samples = 0;
    // Two latency distributions: pooled is the real one, warm is the warm up's, which is looked
    // at but never reported.
    hist::Hist pooled, warm;
    // The loop below gathers in two different ways, which must not be confused.
    // Most are added up - each shard did its own work and the results add.
    // But messages and window_messages are assigned.
    // Every shard went through all the messages, so adding them would double count.
    for (const auto& s : shards) {
        sent += s->sent;
        refused += s->refused;
        stamped += s->stamped;
        no_slot += s->no_slot;
        wnd_block += s->wnd_block;
        if (s->wnd_min < wnd_min) wnd_min = s->wnd_min;
        if (s->wnd_max > wnd_max) wnd_max = s->wnd_max;
        warmed += s->warmed;
        // Assigned rather than added - every shard went through all the messages.
        window_messages = s->window_messages;  // every shard sees all of them
        paced_orders += s->paced_orders;
        // The windows are the same for every shard, so this is assigned as well.
        windows = s->windows;
        windows_dropped = s->windows_dropped;
        dropped_samples = s->dropped_samples;
        // Histograms are merged, which adds their buckets one by one.
        // It must not become "work out each shard's percentiles and average them" - that would
        // give a shard with few samples too much weight.
        warm.merge(s->warmup);
        // The real one. The tick to trade line printed at the end is worked out from it.
        pooled.merge(s->latency);
        // Assigned as well, for the same reason as above.
        messages = s->messages;  // every shard sees every message
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

    // What follows is the report printed at the end of a run.
    // The suggested order to read it in is: first the few numbers that have to be zero, to see
    // whether this round can be used at all, and then the tick to trade lines.
    // A round can be used when all of these are zero:
    //   gaps / lapped / orphan / discards / table full / refused
    //   0 did not fit / thrown away
    // and the card's own drop counters have not moved.
    //
    // How many packets arrived altogether.
    // It is both market data paths together rather than one - the exchange sends the same
    // content twice and we remove the duplicates by sequence number, so this is about twice the
    // number of packets of content.
    std::printf("packets        %" PRIu64 "\n", packets);
    // How many ITCH messages there were. Every shard saw all of them, so this is not a sum.
    std::printf("messages       %" PRIu64 " (each shard saw all of them)\n", messages);
    // How many of them really changed the book. Over a whole day it is about 11% of the total.
    std::printf("applied        %" PRIu64 " across the shards\n", applied);
    // The order rate: orders sent inside windows over the messages inside windows that really
    // changed the book.
    // This line answers how much of the market our own volume is; the smaller it is the closer
    // to real market making.
    std::printf("order rate     %" PRIu64 " stamped over %" PRIu64
                " in-window book touches, %.3f%%\n",
                stamped, applied_window,
                applied_window ? 100.0 * double(stamped) / double(applied_window) : 0.0);
    // How many orders are still resting on the book at the end.
    // It is not zero after the close - the exchange does not cancel every one of them.
    std::printf("orders alive   %" PRIu64 "\n", live);
    // What each of the two ways of building the book cost. There is only something to print
    // with --split-ab.
    if (shards[0]->split_ab) {
        // How many ticks the counter takes per nanosecond, calibrated once at start up, which
        // turns ticks into nanoseconds.
        const double tps = tsc::ticks_per_ns();
        // The indices match the arrays in Shard: 0 is one pass, 1 is two.
        static const char* const path[2] = {"apply as parsed  ", "collect then apply"};
        for (std::size_t w = 0; w < 2; ++w) {
            // A path that never changed the book prints nothing, which also avoids dividing by
            // zero.
            if (shards[0]->ab_msgs[w] == 0) continue;
            std::printf("split %s %" PRIu64 " polls, %.2f book touches each, "
                        "%.1f ns a touch\n",
                        path[w], shards[0]->ab_polls[w],
                        double(shards[0]->ab_msgs[w]) / double(shards[0]->ab_polls[w]),
                        double(shards[0]->ab_ticks[w]) / tps /
                            double(shards[0]->ab_msgs[w]));
        }
    }
    // How many securities really got a price space, which is how many can be traded this round.
    // Without this line there can be a situation where every other check passes and not one
    // order goes out, with nothing at all to say why.
    std::printf("bound          %" PRIu64 " securities, %" PRIu64 " without a price space\n",
                bound, unbound);
    // How long this round ran, and how many messages a second on average.
    // The rate mixes the full speed stretch and the 1:1 stretch together, so it is neither how
    // fast we can go nor how fast the market is. It is only for estimating wall clock.
    std::printf("rate           %.3f s, %.2f M messages/s\n", wall,
                wall > 0 ? messages / wall / 1e6 : 0.0);
    // How many times the strategy said to send, split into buys and sells.
    // The two should be close. Far apart means something is wrong with the threshold or the
    // reference prices - a strategy that only ever leans one way gives a lopsided latency
    // distribution too.
    // The last number is what really went out: signals in the full speed stretch never send.
    std::printf("signals        %" PRIu64 " (buy %" PRIu64 ", sell %" PRIu64 ")"
                ", of them in the paced stretch %" PRIu64 "\n",
                buys + sells, buys, sells, paced_orders);
    // How many messages there were inside windows, how many windows there were, and how many
    // were thrown away.
    // The thrown away number has to be zero.
    // A window is thrown away because we had not caught up with the previous stretch's backlog
    // before it began - and then what is measured is the backlog, not a latency.
    // The window count is a known quantity: the original file at &31 gives 681 and the thinned
    // file at &127 gives 171. A different number means the data file or the mask is wrong.
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
        // Of the orders that did not go out, how many were stopped because the far end said it
        // could take no more, and how large the window it reported was at those moments. A
        // window pinned at zero means the far end reads slowly; a window that stays large means
        // the cause is something else.
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
        // The raw samples are written out one by one. Only with them can the distribution be
        // drawn afterwards, or two stretches of a morning compared, without replaying a whole
        // day again.
        if (opt.out != nullptr) {
            // One row per window. That way the worst moments of a day can be placed rather than
            // merely counted.
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
            // This is where latency.csv is written.
            // It is what scripts/percentiles.py reads to recompute the tail percentiles.
            //
            // The samples are sorted by window before being written.
            // The file still has one column, but knowing which row each window starts at makes
            // it possible to cut by window.
            sample::Log all;
            // Taken all at once. One extra so that a sample count of exactly zero does not ask
            // for zero bytes.
            all.reserve(pooled.samples() + 1);
            // How many samples were lost because the buffer filled. A non zero means this round
            // only covers the first part.
            std::uint64_t lost = 0;
            // Flattened into a row of window number and latency, sorted, and then poured into
            // the file.
            std::vector<std::pair<std::uint16_t, std::uint64_t>> by_window;
            by_window.reserve(pooled.samples() + 1);
            for (const auto& s : shards) {
                for (std::size_t i = 0; i < s->raw.size() && i < s->raw_window.size(); ++i) {
                    by_window.emplace_back(s->raw_window[i], s->raw.data()[i]);
                }
                lost += s->raw.over();
            }
            // stable_sort rather than sort.
            // Samples within one window have to keep the order they were in - that order is the
            // order they were recorded in, and changing it hides how things moved inside the
            // window.
            // With one shard the sort does nothing at all, since the window numbers already
            // increase; with several it interleaves the shards' samples by window.
            std::stable_sort(by_window.begin(), by_window.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            // Once sorted, only the latency column is poured in. The window numbers are not
            // written to the file.
            for (const auto& e : by_window) all.add(e.second);
            if (!sample::write_csv(opt.out, "latency.csv", all)) {
                std::fprintf(stderr, "could not write %s/latency.csv\n", opt.out);
            } else {
                std::printf("samples        %s/latency.csv, %zu rows, %" PRIu64
                            " did not fit\n", opt.out, all.size(), lost);
            }
            // This is where events.csv is written.
            // It is what scripts/segments.py reads to split the four parts 1-0, 2-1, 3-2 and
            // 4-3.
            //
            // It holds the same samples, but every row carries what was going on around it.
            // This file is in the order things were recorded rather than by window, because the
            // question asked of it is exactly what it looks like laid out by arrival time.
            // In one line: latency.csv answers how fast, and this one answers what else was
            // happening at that moment.
            std::FILE* e = std::fopen((std::string(opt.out) + "/events.csv").c_str(), "w");
            if (e != nullptr) {
                // The first line says how many ticks the processor's counter takes per
                // nanosecond on this machine.
                // The tsc columns below are raw counter readings that were never converted.
                // Whoever reads the file divides by this number - that way no precision is lost
                // to a conversion inside the file.
                std::fprintf(e, "# ticks_per_ns %.6f\n", tsc::ticks_per_ns());
                // What the columns mean:
                //   window      which measurement window
                //   rx_ns       when the card received that packet, on the card's clock
                //   latency_ns  this order's wire to wire
                //   poll_n      how many packets that poll took
                //   poll_i      which order of that poll this is, counting from 0
                //   sym         which security
                //   poll_tsc    when the poll returned            <- point 1 of the breakdown
                //   body_tsc    when the packet body was in hand  <- point 2
                //   parse_tsc   when the packets had all been parsed  <- point 2a
                //   book_tsc    when the book had all been changed    <- point 2b
                //   in_tsc      when it entered the send path
                //   out_tsc     when the whole frame was handed over
                //   fill_tsc    when this order's bytes were written
                //   tcp_tsc     when the TCP header and checksums were written  <- point 3
                //   ring_tsc    when it was handed to the card
                // The last seven columns are the processor's counter while rx_ns is the card's
                // clock.
                // There is an unknown fixed offset between the two, so subtracting one from the
                // other directly means nothing - they have to be lined up first by taking the
                // smallest value in a sliding window.
                std::fputs("window,rx_ns,latency_ns,poll_n,poll_i,sym,"
                           "poll_tsc,body_tsc,parse_tsc,book_tsc,"
                           "in_tsc,out_tsc,fill_tsc,tcp_tsc,ring_tsc\n", e);
                std::uint64_t rows = 0;
                for (const auto& s : shards) {
                    // Whichever of the two is smaller.
                    // Those rows are pushed separately and may stop at different places when the
                    // buffer fills; following the longer one would read entries never written.
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
            // The warm up curve: every order sent during a warm up, against how long there was
            // until the window began.
            // Wherever the percentiles stop falling is how long a warm up should be.
            // The present 500 milliseconds was chosen by hand, and this file is what checks it.
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
    // How much had gathered in the card's queue on each poll that found something.
    // One at a time means we are keeping up, and also that there is nothing to batch;
    // a p99 of dozens means a backlog builds - and that is where the tail of the latency comes
    // from.
    {
        std::uint64_t polls = 0, biggest = 0;
        for (int k = 1; k < 65; ++k) {
            polls += shards[0]->depth[k];
            if (shards[0]->depth[k] != 0) biggest = static_cast<std::uint64_t>(k);
        }
        if (polls != 0) {
            // One walk through, taking each percentile as it is passed - rather than searching
            // the same row of 64 numbers four times over.
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
    // The line above counts by poll, and this one counts by packet. They differ greatly:
    //
    // Counted by poll, the shallow polls of the quiet stretches - the vast majority - carry the
    // result away entirely and it reads as though nothing ever arrived in a batch.
    // But what really causes the tail is at the other end: a poll that takes sixty four packets
    // does sixty four times the work of one that takes a single packet, and counting by poll
    // erases it.
    //
    // So this line asks a different question - not what a poll looks like, but what situation a
    // packet finds itself in when it is picked up.
    // Whether batching is worth doing is decided by this line, not by the one above.
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
    // The raw records go to disk. One row per poll and nothing merged - percentiles and cutting
    // by time of day are left to the scripts, so that the same data can be looked at from
    // different angles again and again.
    if (opt.stat != nullptr && shards[0]->stat_used != 0) {
        // The directories are made one level at a time. mkdir makes a single level, and what is
        // given is usually a two level path such as stat/<date>, where making only the last
        // level fails because the one above does not exist.
        // A failure carries on anyway: what really speaks is the result of the fopen below.
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
        // A failure to open has to be said out loud. This used to print only on success, so
        // when the directory could not be made a whole round said nothing, sixty million
        // records quietly vanished, and the file turned out to be missing only afterwards.
        if (sf == nullptr) std::perror(path.c_str());
        if (sf != nullptr) {
            // This is polls_raw.csv, one row per poll.
            // scripts/poll_stat.py reads it to answer how many messages a poll has in hand.
            //
            // What the columns mean:
            //   day_ns       the market time of the first message of this batch, in nanoseconds
            //                from midnight
            //   packets      how many packets this poll took
            //   itch_msgs    how many ITCH messages there were in them altogether
            //   rx_span_ns   how far apart the earliest and latest arrivals of this batch were
            //   idle_ns      how long was spent spinning after the previous poll before
            //                anything arrived
            //   proc_ns      how long handling this batch took
            //   send_ns      how long handing the orders to the card took
            //   raw_*_tsc    three raw readings of the processor's counter, never converted
            //   raw_nic_ns   the card's timestamp on the first packet
            //   blind_ns     how long we went without polling while this batch was handled
            // The first seven columns are nanoseconds worked out here and the last five are raw
            // - the raw ones are left for the scripts to line up themselves, because the card
            // and the processor are two clocks and how to line them up depends on the question.
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
        // The three things done on the empty path: how long each took at worst and how often it
        // went past ten microseconds.
        // That path is ninety nine percent of the time and any stall is there too, so they are
        // the first suspects.
        // These three are the first thing to look at when hunting where the tail comes from: a
        // worst value of tens of microseconds in any of them means the tail is not in the
        // processing but here.
        static const char* what[3] = {"drained.fetch_add", "reap", "resend_stale"};
        for (int q = 0; q < 3; ++q) {
            std::printf("empty path     %-18s worst %" PRIu64 " ns, over 10us %" PRIu64
                        " times\n", what[q], shards[0]->empty_worst[q],
                        shards[0]->empty_over[q]);
        }
    }
    // Whether the card's own drop counters moved.
    // Three outcomes have to be told apart: could not be read, did not move, moved.
    // "Could not be read" must not become "did not move" - that would quietly skip a check.
    std::printf("card counters  %s\n",
                !have_counters ? "could not be read" : (clean ? "unchanged" : "MOVED"));
    // If they really moved, which ones and from what to what.
    if (have_counters && !clean) (void)nic::report_drops(before, after);
    // The exit code is this round's verdict.
    // All seven have to pass for a 0, and any one failing gives a 1.
    // That way a script running many rounds, such as full_day.sh, does not have to read the
    // log. The exit code says whether to keep the round.
    //
    // The seven are:
    //   gaps            a hole in the sequence numbers, so packets really were missed
    //   lapped          lapped, meaning the handling could not keep up with the publishing
    //   orphan          a message mentioning an order id we do not hold
    //   discards        the card received something with nowhere to put it, or decided to
    //                   throw it away
    //   full            the order table filled
    //   windows_dropped a window was thrown away for not having caught up before it began
    //   the card's counters   not one of its four drop counters moved
    // The last is written as (clean || !have_counters), so being unable to read them is not a
    // failure.
    // In some environments there is simply no permission to read those counters.
    // The other six are enough to judge by then.
    return (gaps == 0 && lapped == 0 && orphan == 0 && discards == 0 && full == 0 &&
            windows_dropped == 0 && (clean || !have_counters))
               ? 0
               : 1;
}
