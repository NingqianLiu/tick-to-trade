#pragma once

// How the market data one core receives is handed to the other cores.
//
// Why this exists at all. Seeing "several cores handle the market data", the natural thought is
// that every core fetches packets from the card itself. That cannot be done - the card's event
// queue allows only one consumer, and one core in the whole machine may poll it.
// So the division is: that one core does nothing else, taking out of each event where the packet
// is, how long it is, and the timestamp the card wrote, and publishing those three; it parses no
// messages and judges nobody's interest in them.
// Every other core reads every publication and fetches the content from the memory the card
// wrote into.
//
// In our terms the polling one is the polling thread and the readers are the working threads.
// On the single shard path both jobs happen on one core and none of this is used.
//
// Why "move it once and let everyone read" is right:
// moving is serial. There is one PCIe link and nobody beats it.
// Reading can be parallel.
// So it is moved once and then everyone is let loose to read.
//
// The easiest thing to get wrong in the design is this: the sequence number lives in the entry.
// Rather than in a separate counter saying how far the writer has got.
//
// A shared counter seems simplest.
// But it would then be the one cache line in the whole system that every core shares.
// Every publication would invalidate the copy every reader holds. The more cores, the worse.
//
// With the number in the entry, a reader watches the entry it wants next.
// And the entry the writer touches is one nobody is waiting on. The two never meet.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ring {

// One published record, thirty two bytes, so a cache line of 64 holds exactly two.
//
// What matters here is not the fields but which write counts as publishing: the write to seq.
// It does two things at once - it fills in the number, and it guarantees the four fields above
// were written before another core can see that number.
struct Descriptor {
    // Which record this entry holds. Zero means this entry has never been written.
    std::atomic<std::uint64_t> seq;
    // Where the packet's content is. It is the memory the card wrote into rather than a copy, so
    // what a reader reads is the same bytes the card wrote.
    const std::uint8_t* buf;
    // 32 bits rather than 16, not because a packet can exceed sixty thousand bytes but so that
    // the whole record comes to exactly thirty two - which is what the static_assert below is
    // about.
    std::uint32_t len;
    // Always zero at present. It stays because removing it would make this record no longer
    // thirty two bytes, and that size is deliberate; keeping it also means adding something
    // later does not change the layout.
    std::uint32_t flags;
    // The start of the whole latency chain. It comes from the card's own crystal, and the
    // processor's timestamp counter is a different clock with an unknown fixed offset between
    // them - subtracting one from the other directly means nothing, and they have to be lined up
    // first by taking the smallest value in a sliding window.
    std::uint64_t hw_ts;
};
// The thirty two bytes are nailed down.
// Not fussiness: the moment somebody adds a field and it becomes thirty three, a cache line
// holds one record instead of two and a reader walking them fetches memory once more per record
// - the whole memory behaviour changes, and nothing reports it.
// So it is stopped at compile time.
static_assert(sizeof(Descriptor) == 32);

// What a reader receives.
// The four small fields are copied while the packet's content is only a pointer.
// That is not casual: the small fields are small and copying them is cheaper than going back to
// the entry to read again, and once copied it does not matter if the entry is overwritten; the
// packet's content is a thousand odd bytes and copying that really would be dear.
struct View {
    // It still points at the memory the card wrote into rather than a copy. So a reader has to
    // finish reading before the writer comes round and overwrites that entry - kLapped below is
    // the signal that this was not done.
    const std::uint8_t* buf = nullptr;
    // How many bytes from that pointer are valid. Past that number is another packet.
    std::uint32_t len = 0;
    // The spare bits from the publisher, always zero at present.
    std::uint32_t flags = 0;
    // The arrival time the card stamped. A reader works out latency entirely from it, so it is
    // copied even where it is not used at present.
    std::uint64_t hw_ts = 0;
};

// A ring of entries. One writer, many readers, and no locks for anyone.
class Ring {
public:
    // Construction. It takes how many entries are wanted and rounds up to a power of two.
    //
    // Why it has to be a power of two: coming round to the start is then a bitwise and rather
    //    than a modulo.
    //    A modulo on this path is dozens of cycles and a bitwise and is one.
    // Why it has to be large enough: so that the writer coming round to a given entry happens
    //    late enough - late enough that the entry had long fallen out of a reader's cache
    //    anyway. Too small, and every lap broadcasts a cache invalidation to every core, which
    //    is exactly what this design is trying to avoid.
    explicit Ring(std::size_t slots) : slots_(round_up(slots)), mask_(slots_ - 1) {
        // Every entry is allocated at once. Nothing more is asked of the system once the run is
        // going.
        d_ = std::vector<Descriptor>(slots_);
        // All cleared to zero, meaning never written.
        // Relaxed ordering is enough here - no reader is running yet and nobody can see a
        // half-made state.
        for (auto& d : d_) d.seq.store(0, std::memory_order_relaxed);
    }

    // How many entries there really are, after rounding up to a power of two. For printing and
    // for checking.
    [[nodiscard]] std::size_t slots() const noexcept { return slots_; }

    // Publish one record. Only the core polling the card calls this.
    // The numbers count from one, so an entry still holding zero was certainly never written and
    // a reader can never take something left over from a previous lap as real data.
    void publish(const std::uint8_t* buf, std::uint32_t len, std::uint64_t hw_ts,
                 std::uint32_t flags = 0) noexcept {
        // The mask gives which entry this record lands in, which is the coming round above.
        Descriptor& d = d_[next_ & mask_];
        // The order of these four lines does not matter, but all of them have to come before the
        // write to seq below.
        // Until seq has been written, this entry is still "the previous lap's old data" as far
        // as a reader is concerned and nobody comes to read it - so these four can be written in
        // any order and need no synchronisation.
        d.buf = buf;
        // How many bytes are valid. A reader uses it to decide where to stop.
        d.len = len;
        // The spare bits, passed through as they are.
        d.flags = flags;
        // The card's arrival time, also passed through. Nothing is converted here - every extra
        // piece of work on the polling core adds to the wait of every packet after it.
        d.hw_ts = hw_ts;
        // This is what publishing means. A release write means: the writes to the four fields
        // above are guaranteed complete before another core can see the new number.
        // Without that guarantee the processor or the compiler is quite free to move the write
        // to seq earlier, and a reader would see the new number and then read content not
        // finished being written - a bug of very low probability and almost impossible to track
        // down, so it is shut off here.
        d.seq.store(next_, std::memory_order_release);
        // The number moves on. Only the publishing core touches this variable, so it does not
        // have to be atomic.
        ++next_;
    }

    // How many records have been published. The closing figures and "how far behind a reader is"
    // both use it.
    [[nodiscard]] std::uint64_t published() const noexcept { return next_ - 1; }

    // The three outcomes of reading an entry:
    // kReady   the record wanted is in this entry and its content has been put into the View.
    // kWaiting not yet; this entry still holds an earlier record - keep waiting.
    // kLapped  lapped. The writer has come all the way round and this record can never be read.
    //          Messages really were lost.
    enum class State : std::uint8_t { kReady, kWaiting, kLapped };

    // This is the body for a reader: ask whether the record wanted has arrived.
    // It takes which record is wanted and a View to fill in; it returns one of the three states
    // above, and only kReady means the View was filled.
    //
    // The order matters: the number has to be read and matched before any other field is
    // touched. Reading the content first could read what the previous lap left in that entry,
    // and it would look perfectly normal.
    [[nodiscard]] State take(std::uint64_t want, View* out) const noexcept {
        // The record wanted should be in this entry.
        const Descriptor& d = d_[want & mask_];
        // An acquire read, which pairs exactly with the publisher's release write: once this
        // number has been read, the content of the four fields above is certainly visible too
        // and a half-written state cannot be read.
        const std::uint64_t here = d.seq.load(std::memory_order_acquire);
        // The number is exactly the record wanted, so the content is in place.
        if (here == want) {
            // The packet's content is still in the memory the card wrote into and most likely
            // not in cache, taking hundreds of cycles to fetch. A prefetch hint goes out first so
            // that the fetch overlaps with the work between here and really reading it. It is
            // only a hint and does not wait for the data, so this line itself costs almost
            // nothing.
            __builtin_prefetch(d.buf, 0, 3);
            // These four fields are copied away rather than left as a pointer to read later.
            // Once copied, even if the writer comes round and overwrites this entry at once,
            // the four numbers the reader holds are still right.
            out->buf = d.buf;
            // How many bytes are valid.
            out->len = d.len;
            // The spare bits.
            out->flags = d.flags;
            // The card's arrival time. With these four copied the caller can go and parse the
            // packet - only the packet's content is still where it was, and that is the part
            // there is really a race to finish reading.
            out->hw_ts = d.hw_ts;
            // Tell the caller the content is valid.
            return State::kReady;
        }
        // Reaching here the number does not match, which is one of two things:
        // this entry already holds a later record - the writer came all the way round while this
        //   reader was behind and the record wanted was overwritten, which really is lost
        //   messages and the caller has to record it;
        // otherwise it is simply not there yet and the reader asks again.
        return here > want ? State::kLapped : State::kWaiting;
    }

private:
    // Rounds the number of entries asked for up to a power of two.
    static std::size_t round_up(std::size_t n) {
        // Starting at two. It cannot start at one: the mask is the number of entries less one,
        // and with one entry the mask is zero and every message lands in the same entry.
        std::size_t p = 2;
        // Doubled until it is at least what was asked for. A number that is already a power of
        // two doubles not at all.
        while (p < n) p <<= 1;
        // The constructor uses it to settle both the number of entries and the mask, which have
        // to come from the same value.
        return p;
    }

    // Every entry. Allocated at construction and never resized.
    std::vector<Descriptor> d_;
    // The number of entries and its mask, which is that number less one. Coming round is a
    // bitwise and with this mask.
    std::size_t slots_, mask_;
    // The number of the next record to publish. It starts at one, which leaves zero to mean
    // never written.
    std::uint64_t next_ = 1;  // only the publishing core touches this
};

// How far one reader has got.
// It has a whole cache line to itself, through alignas(64) and the padding after it.
// Two readers' positions sharing one line would dirty each other's cache line once per message -
// which is exactly what the start of this file goes to such lengths to avoid, and missing it
// here would undo all of it.
struct alignas(64) Cursor {
    // Which record is wanted next. Counting from one as the publisher does.
    std::uint64_t want = 1;
    // Fills the rest of the cache line so that nobody else can move in.
    char pad[56];
};
// The whole cache line size is nailed down, so that adding a field later cannot get the padding
// wrong without anyone noticing.
static_assert(sizeof(Cursor) == 64);

}  // namespace ring
