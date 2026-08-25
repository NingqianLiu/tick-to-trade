#pragma once

// A block of memory asked for in 2 MB pages, for the arrays of the book that live a whole round.
//
// Who uses it: the order table (pool_map), the price space (price_levels), and the card's
// receive buffers (Frames in net/ef.hpp).
// Those three together are most of this process's memory.
//
// Why huge pages are necessary:
// the order table and the price space together are 8.5 GB.
// In the ordinary 4 KB pages that is two million one hundred thousand pages.
// And the second level cache of the address translation holds 2,048 entries.
// So any lookup that lands somewhere not touched recently has to walk the page table first, and
// only then does the fetch it actually wanted begin.
// In 2 MB pages the number of pages is divided by five hundred.
//
// Huge pages have to be reserved per node before a run. Without a reservation this falls back to
// ordinary pages.
// It says so out loud when it falls back, and that matters:
// a round that quietly fell back to 4 KB pages differs from one that did not in the latency
// numbers alone.
// And the latency numbers are the only thing here that is ever used as evidence.

#include <dirent.h>
#include <linux/mman.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace huge {

// A page is 2 MB.
// Every rounding and page count in this file works from it.
// 1 GB pages are not used, because this processor's address translation cache has no room for
// them.
inline constexpr std::size_t kPage = 2u << 20;

// Which half of the memory the pages come from. Settled once at start up and never changed
// after.
//
// What happens without naming it:
// the kernel gives a page to the half the first thread to touch it is on.
// And that is exactly where it goes wrong.
// The thread that first writes the book into memory is not necessarily pinned yet.
// So part of the book lands on the far half.
// Or worse: it empties the pool another process needed. That is how this was found.
// Naming the node plainly takes it out of the scheduler's hands.
inline int& node() noexcept {
    // A static inside a function rather than a global, so it is certainly initialised before its
    // first use.
    // -1 means nobody has named one.
    static int chosen = -1;
    return chosen;
}

// Which half a cpu belongs to.
// Read out of sysfs rather than written into the code as a table.
// That way a different machine, or a different numbering on this one, changes the answer by
// itself.
[[nodiscard]] inline int node_of_cpu(int cpu) {
    // Build that cpu's sysfs path.
    char path[64];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
    // Open it. Unable to open returns -1 and leaves the decision to the caller.
    DIR* d = ::opendir(path);
    if (d == nullptr) return -1;
    // Not found yet.
    int found = -1;
    // Inside this directory is a link called nodeN, and N is the answer.
    while (const dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, "node", 4) == 0 && e->d_name[4] >= '0' &&
            e->d_name[4] <= '9') {
            // The first four characters are node and a digit follows, so this is the one.
            found = std::atoi(e->d_name + 4);
            break;
        }
    }
    // The directory is closed, since this function may be called several times during start up.
    ::closedir(d);
    // Not found returns -1.
    return found;
}

// Sets the node to n outright, with no checking at all.
// Only choose() and reserve() below call it, and it should not be used from outside.
inline void bind_to(int n) noexcept { node() = n; }

// Names the node. It has to be called before a single page is taken, and stops if any has been.
//
// Why it is so strict:
// with nobody naming one, reserve() below settles for the half the current thread is on.
// That is for the tools that never name a node, and they still get the right memory.
// The cost is: a tool that really did name one, but too late, hears not a word of complaint.
// Because by then the pages are somewhere and mbind has tied them there.
// And the check for "is every page where it should be" then compares a wrong answer against
// itself.
// It passes, naturally.
// That is how four gigabytes of receive buffers ended up on the far half.
//
// So what is really being checked is the order itself:
// the node already having a value on reaching here means the pages went out first.
// And by then, naming the right node moves not one page back.
inline void choose(int n) {
    // Somebody named one already, or pages have been taken already.
    if (node() >= 0) {
        std::fprintf(stderr,
                     "huge pages were already taken from node %d before node %d "
                     "was named; something mapped before the choice was made\n",
                     node(), n);
        // It stops. Carrying on would measure the speed of the far half in this round, and that
        // number looks exactly like the thing being measured, with no telling them apart
        // afterwards.
        std::abort();
    }
    // The order is right, so the node is recorded.
    bind_to(n);
}

// Reads and writes the huge page pool of the chosen half.
//
// Why the program opens and returns it as it goes rather than reserving once at boot:
// the pages a pool holds cannot be used by anything else on this machine.
// And a pool left over from a previous round holds pages taken when the memory looked as it did
// then.
// 2 MB pages can always be had again on this machine, so there is no reason to hold them across
// rounds.
inline bool pool(const char* leaf, std::size_t* value, bool write) {
    // Build the path of that file in sysfs.
    // leaf is either nr_hugepages or free_hugepages.
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/node/node%d/hugepages/hugepages-2048kB/%s",
                  node(), leaf);
    // Opened for reading or for writing as asked.
    std::FILE* f = std::fopen(path, write ? "w" : "r");
    // Unable to open usually means no root permission, or that this machine has no such half.
    if (f == nullptr) return false;
    // Writing puts value in, and reading fills value with what was read.
    const bool ok = write ? std::fprintf(f, "%zu", *value) > 0
                          : std::fscanf(f, "%zu", value) == 1;
    std::fclose(f);
    return ok;
}

// How large the chosen half's pool is now.
// Anyone wanting to confirm the pool went back to what it was reads this before and after.
[[nodiscard]] inline std::size_t pool_size() {
    std::size_t nr = 0;
    // Unable to read counts as zero. The caller compares it with the figure from before the run
    // to see whether things are clean.
    return pool("nr_hugepages", &nr, false) ? nr : 0;
}

// How many pages this process added to the pool.
// It is remembered so that exactly that many go back at the end, neither more nor fewer.
// Returning more would take back pages somebody else added.
inline std::size_t& borrowed() noexcept {
    static std::size_t n = 0;
    return n;
}

// Gives the borrowed pages back.
// Called automatically when the process ends, through the atexit in reserve below.
inline void give_back() {
    // Nothing was borrowed, so there is nothing to give back.
    if (borrowed() == 0) return;
    std::size_t nr = 0;
    // Read how large the pool is now first.
    if (pool("nr_hugepages", &nr, false)) {
        // Take off what we borrowed. Clamped at zero, so that somebody else having shrunk the
        // pool does not work out a negative number that wraps round.
        std::size_t left = nr > borrowed() ? nr - borrowed() : 0;
        pool("nr_hugepages", &left, true);
    }
    // Back to zero, so that calling it again does not give them back twice.
    borrowed() = 0;
}

// Grows the pool to what is needed.
// A false means the kernel could not put that many pages together.
// That is a reason not to start the run, not a reason to run more slowly.
[[nodiscard]] inline bool reserve(std::size_t pages) {
    // With nobody naming a node, the half the current thread is on is used.
    // So every tool that opens the card takes pages from the right place without remembering to
    // name one.
    // And a tool that really did name one still gets what it asked for.
    if (node() < 0) bind_to(node_of_cpu(::sched_getcpu()));
    // Unable even to find which half the current cpu is on means going no further.
    if (node() < 0) return false;
    // nr is how large the pool is altogether and free_now is how many of them are not in use.
    std::size_t nr = 0, free_now = 0;
    if (!pool("nr_hugepages", &nr, false)) return false;
    if (!pool("free_hugepages", &free_now, false)) return false;
    // The pool holds pages, not one of them is in use, and we have borrowed none ourselves.
    // That is what a previous round left behind when it died before giving them back.
    // The whole thing is cleared and asked for afresh, rather than holding a pile of pages taken
    // when the memory looked as it did then.
    if (borrowed() == 0 && nr > 0 && free_now == nr) {
        std::size_t zero = 0;
        pool("nr_hugepages", &zero, true);
        nr = free_now = 0;
    }
    // There are not enough free pages.
    if (free_now < pages) {
        // Grow the pool to what it is now plus what is missing.
        std::size_t want = nr + (pages - free_now);
        if (!pool("nr_hugepages", &want, true)) return false;
        // How many were borrowed is recorded, so exactly that many go back at the end.
        borrowed() += pages - free_now;
        // A successful write is not the same as really having them.
        // The kernel may not be able to put continuous physical memory together, so the pool did
        // not really grow.
        // So it is read again to confirm.
        if (!pool("free_hugepages", &free_now, false) || free_now < pages) {
            // Not enough. What was borrowed goes back rather than leaving half of it sitting
            // there.
            give_back();
            return false;
        }
    }
    // One callback for the exit is registered, so the pages go back by themselves when the
    // process ends.
    // A static flag keeps it to one registration - reserve is called several times in a round.
    static bool armed = false;
    if (!armed) {
        std::atexit(give_back);
        armed = true;
    }
    return true;
}

// This is the body of the file: ask for a block of a whole number of huge pages, tied to the
// named half.
// Unable to get them it falls back to ordinary pages, but says so out loud rather than falling
// back quietly.
// bytes is passed by pointer: it is rounded up to whole pages in place and the caller has to use
// the new value.
[[nodiscard]] inline void* map(std::size_t* bytes) {
    // Rounded up to whole pages.
    *bytes = (*bytes + kPage - 1) / kPage * kPage;
    // Asking for zero bytes means nothing and is turned away.
    if (*bytes == 0) return nullptr;
    // Failure is assumed first, and the two paths below each try once.
    void* m = MAP_FAILED;
    // Enough pages are prepared in the pool before mapping.
    // MAP_HUGE_2MB fixes the page size rather than taking whatever the machine's default is.
    // That way a machine set up for 1 GB pages does not quietly give a 1 GB page where 2 MB were
    // wanted.
    if (reserve(*bytes / kPage)) {
        m = ::mmap(nullptr, *bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    }
    // The huge page path failed.
    if (m == MAP_FAILED) {
        // It says so, but once a round - several blocks are mapped in a round and saying it
        // every time would fill the screen.
        // This line matters: a round that quietly fell back to 4 KB pages differs from one that
        // did not in the latency numbers alone, and those are exactly what is used as evidence.
        static bool said = false;
        if (!said) {
            std::fprintf(stderr, "2 MB pages refused, falling back to 4 KB pages\n");
            said = true;
        }
        // Falling back to ordinary pages. The memory is still given, because not running at all
        // is worse than running slowly.
        m = ::mmap(nullptr, *bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    // Neither path worked, so there really is no memory.
    if (m == MAP_FAILED) return nullptr;
    // The block is tied to the named half.
    if (node() >= 0) {
        // Those two numbers are MPOL_BIND and MPOL_MF_STRICT.
        // They are written as numbers here rather than including <linux/mempolicy.h>, because
        // the machines this code is built on do not all have that header.
        const unsigned long mask = 1ul << node();
        if (::syscall(SYS_mbind, m, *bytes, 2, &mask, 8 * sizeof(mask), 1u) != 0) {
            // Failing to tie it only says so and does not stop.
            // The real check is must_be_local below: it looks at where the pages ended up rather
            // than at whether this call reported an error.
            std::fprintf(stderr, "cannot tie %zu MB to node %d\n", *bytes >> 20, node());
        }
    }
    // The address goes to the caller. Note that bytes has also been changed to the rounded size.
    return m;
}

// Which half the pages of a block really landed on. It only means something asked after the
// pages have been touched.
// It returns the node of the first page that is not where it should be, and -1 if all of them
// are.
//
// Why it is asked rather than assumed:
// a page lands on the half the first thread to write it is on.
// So getting this wrong needs no more than one thread that is not pinned yet.
// And the only symptom is that round running a little slower - which looks exactly like the
// thing being measured.
[[nodiscard]] inline int stray_page(const void* p, std::size_t bytes) {
    // With no node named, or no memory at all, there is nothing to say.
    if (node() < 0 || p == nullptr || bytes == 0) return -1;
    // How many pages there are.
    const std::size_t n = bytes / kPage;
    // Asked one page at a time.
    for (std::size_t i = 0; i < n; ++i) {
        // Where this page starts.
        void* addr = const_cast<char*>(static_cast<const char*>(p)) + i * kPage;
        int where = -1;
        // A null fourth argument to move_pages means only ask, do not move.
        // Unable to ask gives this check up and treats it as fine.
        if (::syscall(SYS_move_pages, 0, 1, &addr, nullptr, &where, 0) != 0) return -1;
        // One page not where it should be is reported at once, without asking about the rest.
        if (where >= 0 && where != node()) return where;
    }
    // Every page is right.
    return -1;
}

// The same question, asked once, stopping loudly if anything is wrong.
// Carrying on with half the book on the far half measures the speed of that half rather than
// what we set out to measure.
inline void must_be_local(const void* p, std::size_t bytes, const char* what) {
    const int stray = stray_page(p, bytes);
    // No page went astray and there is nothing to do.
    if (stray < 0) return;
    // what is a name the caller gives, such as "card buffers" or "book".
    // Only with it is it clear which block has the problem.
    std::fprintf(stderr, "%s: %zu MB landed on node %d, wanted node %d\n", what,
                 bytes >> 20, stray, node());
    // It stops. Not stopping would let this round quietly produce numbers nothing can be
    // attributed to.
    std::abort();
}

// An array of T in huge pages, used like a small part of a vector.
// The several gigabyte arrays of the book all use it.
template <typename T>
class Buffer {
public:
    // Default construction gives an empty one, with no memory until assign or resize.
    Buffer() = default;
    // Move construction. It swaps with an empty object, so the original becomes empty.
    Buffer(Buffer&& o) noexcept { swap(o); }
    // Move assignment. A swap again - the memory we held is destroyed along with o.
    Buffer& operator=(Buffer&& o) noexcept {
        swap(o);
        return *this;
    }
    // No copying. Duplicating several gigabytes is both pointless and would empty the huge page
    // pool.
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    // Destruction. It only unmaps and does not run the elements' destructors.
    // What is held here is plain numbers and small structures with nothing to clean up.
    ~Buffer() {
        if (p_ != nullptr) ::munmap(p_, bytes_);
    }

    // Asks for n entries, each filled with v.
    void assign(std::size_t n, const T& v) {
        // The memory is asked for first.
        take(n);
        // Every element is constructed in place.
        // This loop is also the touching of every page: after it every page is really in hand
        // and no write while running has to stop and ask the kernel for one.
        for (std::size_t i = 0; i < n; ++i) new (p_ + i) T(v);
        // Where the pages landed can only be asked after they have been touched, so this line
        // has to come after that loop.
        must_be_local(p_, bytes_, "book");
    }
    // The same, only with each entry taking its default value.
    void resize(std::size_t n) {
        take(n);
        for (std::size_t i = 0; i < n; ++i) new (p_ + i) T();
        must_be_local(p_, bytes_, "book");
    }

    // How many elements there are.
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    // The last element, needed by the way a free list is strung together from the back.
    T& back() noexcept { return p_[n_ - 1]; }
    // Taken by index. There is no bounds check - this is used on the hot path and a check would
    // be one more instruction.
    T& operator[](std::size_t i) noexcept { return p_[i]; }
    // The read only version.
    const T& operator[](std::size_t i) const noexcept { return p_[i]; }

private:
    // All three members exchanged together. Move construction and move assignment both rest on
    // it.
    void swap(Buffer& o) noexcept {
        std::swap(p_, o.p_);
        std::swap(bytes_, o.bytes_);
        std::swap(n_, o.n_);
    }

    // Takes a new block of memory.
    void take(std::size_t n) {
        // Anything old in hand goes back first. This class does not grow while keeping the
        // content; taking a new block replaces all of it.
        if (p_ != nullptr) ::munmap(p_, bytes_);
        p_ = nullptr;
        // The number of elements is recorded.
        n_ = n;
        // The byte count is worked out. map rounds it up to whole pages, so what is stored here
        // is what was asked for and the real rounded size is written back by map.
        bytes_ = n * sizeof(T);
        // Asking for none gives an empty one.
        if (bytes_ == 0) return;
        void* m = map(&bytes_);
        // Unable to get memory throws. It is the only place in this file that throws, and it
        // only happens at start up; the hot path can never reach it.
        if (m == nullptr) throw std::bad_alloc();
        p_ = static_cast<T*>(m);
    }

    // Where the memory starts. Null means this one is empty.
    T* p_ = nullptr;
    // In order: how many bytes are really mapped, rounded up to whole pages, and how many
    // elements there are.
    std::size_t bytes_ = 0, n_ = 0;
};

}  // namespace huge
