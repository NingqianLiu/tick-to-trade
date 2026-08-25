#pragma once

// Getting started with the Solarflare card, wrapped in two small classes. Both the trader
// that receives market data and the replay that sends it begin here.
//
// From ordinary network programming, using a card means opening a socket and calling read
// and write. None of that applies. Three things have to be created by hand, one inside the
// next:
//   a handle onto the card's driver
//   a protection domain, which is what says "the card is allowed to touch this memory"
//   a virtual interface of our own, with its own receive ring, transmit ring and event
//   queue
// Once they exist, sending and receiving never enter the kernel: those queues are read and
// written directly.
//
// Huge pages come up throughout. They sound like the kind of tuning that shaves a few
// percent. They are not. Between the card and host memory sits an IOMMU that translates
// every address, and its own translation cache holds only a few dozen entries. A megabyte
// of memory is 256 translations at 4 KB a page, which does not fit, so nearly every packet
// pays a table walk. The same megabyte in 2 MB pages is one translation, which always
// hits. The difference is not a few percent - it is whether each packet takes an extra
// trip.

// munmap. Getting huge pages is common/huge.hpp's job; only giving them back happens here.
#include <sys/mman.h>

// ef_driver_open, ef_driver_close and the ef_driver_handle type.
//
// The name misleads: what is opened is the driver, not the card. It is the handle
// mentioned above, and all three of the objects below have to be asked for with it in
// hand.
#include <etherfabric/base.h>
// ef_memreg, with ef_memreg_alloc and ef_memreg_dma_addr.
//
// The memory is ours already, so why register it? Because "this process may touch it" and
// "the card may touch it" are different things: every access by the card goes through the
// IOMMU, and an address that was never registered is refused. Registering also lets us ask
// what address the card sees for any byte of it.
#include <etherfabric/memreg.h>
// ef_pd, with ef_pd_alloc_by_name and ef_pd_free.
//
// pd is protection domain. The word protection makes it sound like a security feature that
// could be skipped; it is really the registry above. Both the virtual interface and any
// registered memory hang underneath one, so nothing else can start without it.
#include <etherfabric/pd.h>
// ef_vi, with ef_vi_alloc_from_pd, ef_vi_free and the flags enum.
//
// The virtual interface is the whole point: a receive descriptor ring, a transmit
// descriptor ring and an event queue, all three mapped into user space, so the kernel is
// not on the path at all.
#include <etherfabric/vi.h>

#include <cstddef>
// Buffers are addressed as bytes, so the base pointer is std::uint8_t*.
#include <cstdint>
// Every failure in this file prints one line and returns false. All of this happens before
// a run starts, so the caller simply exits; the hot path never reaches any of it.
#include <cstdio>

// huge::map asks for memory in huge pages, and huge::must_be_local checks it landed on the
// near half of the machine. This box has two halves with their own memory, and reaching
// across costs more than three times as much. A card buffer needs both: huge pages for the
// IOMMU, and local memory so the processor reading it does not take the long way round.
#include "common/huge.hpp"
// cfg::kHugePageBytes, the 2 MB step used when touching each page below.
#include "common/settings.hpp"

namespace ef {

// One virtual interface, from being asked for to being given back.
//
// Used as: open() first, and once that succeeds get() hands out the raw pointer to work
// with. All three objects are returned when the object goes out of scope.
class Vi {
public:
    // The default constructor does nothing but clear three "did we get it" flags. Every
    // request lives in open(), because each of the three steps can fail and a constructor
    // has no way to say so.
    Vi() = default;
    // No copying: these are handles onto kernel objects, and a copy would give them back
    // twice. By the second time somebody else may hold the same number, so what would be
    // closed is theirs.
    Vi(const Vi&) = delete;
    // The same, and worse: assigning would also silently leak whatever the left hand side
    // was holding.
    Vi& operator=(const Vi&) = delete;

    // Gives back what open() took, in reverse order of taking it. The interface hangs under
    // the protection domain and the domain was opened with the driver handle, so returning
    // them the other way round is refused.
    ~Vi() {
        // The receive ring, the transmit ring and the event queue go back to the hardware.
        if (has_vi_) ef_vi_free(&vi_, dh_);
        // After this the card may no longer touch anything registered in that domain.
        if (has_pd_) ef_pd_free(&pd_, dh_);
        // And then the handle, after which neither of the two above means anything.
        if (has_dh_) ef_driver_close(dh_);
    }

    // Opens an interface of our own on the port named intf: a receive ring of rxq
    // descriptors, a transmit ring of txq, and a set of flags.
    //
    // The order is driver handle, protection domain, interface. Any step that fails prints
    // a line and returns false; all three succeeding raises the three flags and the caller
    // carries on with get().
    [[nodiscard]] bool open(const char* intf, int rxq, int txq,
                            enum ef_vi_flags flags) {
        // Before this line there is no connection to the card at all. After it, every
        // request below carries this handle, which is how the driver knows what belongs to
        // this process - and how everything is reclaimed if the process dies.
        int rc = ef_driver_open(&dh_);
        // This API does not use errno: zero is success and a negative number is the error,
        // so -13 is a permission problem. Hence < 0 rather than == -1. The usual failure
        // here is the driver not being loaded, or the device file not being readable by
        // this user.
        if (rc < 0) return fail("ef_driver_open", rc);
        // Raised now, so that a failure further down still closes the handle.
        has_dh_ = true;
        // A protection domain on that port.
        //
        // The fourth argument is the kind of domain, and the choice matters more than it
        // looks:
        //   EF_PD_DEFAULT    the ordinary one; the driver maintains the translations and no
        //                    privilege is needed
        //   EF_PD_PHYS_MODE  physical addresses, one translation layer fewer, but it needs
        //                    root - and one address computed wrongly writes straight into
        //                    somebody else's memory
        //   EF_PD_VF         a virtual function on the card, with an IOMMU domain of its
        //                    own; better isolation, but a card has only so many
        // The first, because the buffers are already huge pages so the IOMMU is not under
        // pressure, and the little that physical mode would save is not worth its risk.
        rc = ef_pd_alloc_by_name(&pd_, dh_, intf, EF_PD_DEFAULT);
        // Usually a mistyped port name, or a port that is not a Solarflare card.
        if (rc < 0) return fail("ef_pd_alloc_by_name", rc);
        // The three flags are separate exactly for this: if the next step fails, the
        // destructor gives back the handle and the domain and does not touch an interface
        // that was never created.
        has_pd_ = true;
        // The most important line in the file. After it the card holds a receive
        // descriptor ring, a transmit descriptor ring and an event queue that are ours,
        // with their user space mappings filled into vi_ - and from here on sending and
        // receiving never enter the kernel.
        //
        // The arguments, in order:
        //   &vi_    the interface to fill in; this API has the caller provide the storage
        //   dh_     the driver handle
        //   &pd_    which protection domain it belongs to
        //   dh_     the domain's own handle, the same one here
        //   -1      let the driver pick the interface number; we do not care which
        //   rxq     receive ring entries - entries, not bytes, each holding the address of
        //           one buffer. This is the 4096 from settings.hpp, which was measured
        //           rather than chosen: 4096 is accepted and 8192 is refused. Wanting more
        //           buffering ends here, and the way around it is the 4 GB pool of
        //           buffers, not a deeper ring
        //   txq     transmit ring entries, which is how many frames the card can have in
        //           hand at once
        //   nullptr do not share somebody else's event queue; give this interface its own.
        //           Sharing saves resources across several interfaces, at the cost of
        //           sorting out whose events are whose - and one path with one queue is
        //           exactly what is wanted here
        //   -1      goes with the argument above: no queue is being borrowed
        //   flags   three of which matter for latency:
        //           EF_VI_RX_TIMESTAMPS
        //             the card writes a short prefix before every received packet holding
        //             the hardware time it reached the wire, which is what every latency
        //             here is measured from
        //           EF_VI_TX_TIMESTAMPS
        //             the same for frames going out
        //           EF_VI_RX_EVENT_MERGE
        //             several receive events reported as one, which saves event queue
        //             bandwidth at the cost of the last packet waiting to be announced.
        //             That lands directly on the first segment of the timing - how long
        //             after a packet arrives we are told about it - so it has to be off
        //             while latency is being measured. (The card's own event merge timeout
        //             is also written to zero with sfboot, which is persistent and survives
        //             a reboot.)
        rc = ef_vi_alloc_from_pd(&vi_, dh_, &pd_, dh_, -1, rxq, txq, nullptr, -1,
                                 flags);
        // Usually a ring deeper than one interface on this card allows.
        if (rc < 0) return fail("ef_vi_alloc_from_pd", rc);
        // Raising this also fixes the order of the destructor: it goes back first.
        has_vi_ = true;
        // The caller now takes the raw pointer, posts the receive buffers one entry at a
        // time, installs its filters, and enters the polling loop.
        return true;
    }

    // The raw interface. Every ef_vi call takes it first: polling for events, posting
    // receive buffers, sending.
    [[nodiscard]] ef_vi* get() noexcept { return &vi_; }
    // The driver handle, which Frames below needs when it registers memory - the driver
    // uses it to know the memory and the interface belong together.
    [[nodiscard]] ef_driver_handle dh() const noexcept { return dh_; }
    // The protection domain, also for Frames: memory is only known to the card inside the
    // domain it was registered in.
    [[nodiscard]] ef_pd* pd() noexcept { return &pd_; }

private:
    // All three failures look the same, so each of the three call sites above is one line.
    static bool fail(const char* what, int rc) {
        // Which step failed and with what code. No exception: this all happens before a run
        // starts, so a caller that sees false just exits and there is nothing to recover.
        std::fprintf(stderr, "%s: %d\n", what, rc);
        // Whatever was already taken is left to the destructor, which is what the three
        // flags are for.
        return false;
    }

    // The driver handle: everything above is asked for and released through it.
    ef_driver_handle dh_{};
    // Held by value, not by pointer - this API has the caller provide the storage and the
    // library fill it in.
    ef_pd pd_{};
    // The interface, holding the user space mappings of the rings and the event queue.
    ef_vi vi_{};
    // Kept separately because a middle step can fail, and then only what was really taken
    // may be given back.
    bool has_dh_ = false, has_pd_ = false, has_vi_ = false;
};

// One large piece of huge page memory, cut into equal slots and registered with the card.
//
// On the receive side each slot is posted to the ring and the card writes packets into it.
// On the send side each slot holds the bytes of one frame, and what is handed to the card
// is the address that slot has in the card's view.
class Frames {
public:
    // Everything real happens in alloc(), for the same reason as in Vi.
    Frames() = default;
    // No copying: a mapping and a registration would be undone twice.
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    // Gives the memory back. The registration is not released separately: it hangs under
    // the protection domain, and it goes when Vi above gives that domain back.
    ~Frames() {
        // A null base means alloc() never succeeded and there is nothing to undo.
        if (base_ != nullptr) ::munmap(base_, bytes_);
    }

    // Memory the card can use: which interface it belongs to, how many slots, how large a
    // slot.
    //
    // Four steps - ask for huge pages, touch every page, check it is local, register it -
    // after which at() gives the address the processor sees and dma() the address the card
    // sees, for the same byte.
    [[nodiscard]] bool alloc(Vi& vi, std::size_t slots, std::size_t slot_bytes) {
        slots_ = slots;
        slot_bytes_ = slot_bytes;
        bytes_ = slots * slot_bytes;
        // By address, not by value: the mapping is rounded up to whole 2 MB pages and the
        // real size is written back here.
        void* p = huge::map(&bytes_);
        // Almost always because the system has too few huge pages reserved.
        if (p == nullptr) {
            // How much was wanted and how to provide it, so a failure does not send anyone
            // to the documentation.
            std::fprintf(stderr,
                         "cannot map %zu bytes of huge pages; reserve more with "
                         "sysctl vm.nr_hugepages\n",
                         bytes_);
            // Nothing was mapped, so there is nothing to clean up.
            return false;
        }
        // This class only ever counts bytes from the base.
        base_ = static_cast<std::uint8_t*>(p);
        // One byte written per page.
        //
        // A successful mmap looks like the memory is already in hand; it is only a promise.
        // The page is really allocated, and the page table entry built, on the first write.
        // Leaving that until the run has started would put an allocation in the middle of
        // receiving or sending - a spike of several microseconds against a median of three
        // thousand nanoseconds, and one of those ruins the p99.9 of several windows.
        //
        // One touch per page is enough, so the step is a whole page.
        for (std::size_t off = 0; off < bytes_; off += cfg::kHugePageBytes) {
            base_[off] = 0;
        }
        // The kernel sometimes places pages on the far half of the machine, and then every
        // access takes the link between them, more than three times the distance. The last
        // argument is only a name for the message if something is wrong.
        huge::must_be_local(base_, bytes_, "card buffers");
        // Before this line the IOMMU refuses the card access to this memory. After it, the
        // memory is in this interface's protection domain, the card can read and write it,
        // and ef_memreg_dma_addr can be asked what address any offset has in the card's
        // view.
        //
        // The arguments are the registration to fill in, the driver handle, which domain to
        // register in, the domain's own handle, and the start and length of the memory.
        const int rc = ef_memreg_alloc(&mr_, vi.dh(), vi.pd(), vi.dh(), base_, bytes_);
        // Usually a length that is not aligned, or a domain that has registered as much as
        // it can.
        if (rc < 0) {
            // Only the code; the memory already taken is left to the destructor.
            std::fprintf(stderr, "ef_memreg_alloc: %d\n", rc);
            return false;
        }
        // The caller now posts each slot to the receive ring with dma(i), or writes the
        // bytes of a frame at at(i).
        return true;
    }

    // Where slot number slot is, as the processor sees it: for reading a packet the card
    // just wrote, or writing a frame to send.
    [[nodiscard]] std::uint8_t* at(std::size_t slot) noexcept {
        return base_ + slot * slot_bytes_;
    }
    // Where the same byte is as the card sees it, after the IOMMU. Not a second piece of
    // memory - two views of one. Posting a receive buffer or handing over a frame has to
    // use this one.
    [[nodiscard]] ef_addr dma(std::size_t slot) noexcept {
        return ef_memreg_dma_addr(&mr_, slot * slot_bytes_);
    }
    // How many slots there are, which the receive side walks at start up to post them all.
    [[nodiscard]] std::size_t slots() const noexcept { return slots_; }
    // How many bytes were really mapped, rounded up to whole pages.
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    // Null until alloc() succeeds, which is what the destructor checks.
    std::uint8_t* base_ = nullptr;
    std::size_t bytes_ = 0, slots_ = 0, slot_bytes_ = 0;
    // The registration, which ef_memreg_dma_addr needs to turn an offset into a card
    // address. It hangs under the protection domain, so it is not released here.
    ef_memreg mr_{};
};

}  // namespace ef
