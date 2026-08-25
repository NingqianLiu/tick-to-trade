#pragma once

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

inline constexpr std::size_t kPage = 2u << 20;

inline int& node() noexcept {
    static int chosen = -1;
    return chosen;
}

[[nodiscard]] inline int node_of_cpu(int cpu) {
    char path[64];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
    DIR* d = ::opendir(path);
    if (d == nullptr) return -1;
    int found = -1;
    while (const dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, "node", 4) == 0 && e->d_name[4] >= '0' &&
            e->d_name[4] <= '9') {
            found = std::atoi(e->d_name + 4);
            break;
        }
    }
    ::closedir(d);
    return found;
}

inline void bind_to(int n) noexcept { node() = n; }

inline void choose(int n) {
    if (node() >= 0) {
        std::fprintf(stderr,
                     "huge pages were already taken from node %d before node %d "
                     "was named; something mapped before the choice was made\n",
                     node(), n);
        std::abort();
    }
    bind_to(n);
}

inline bool pool(const char* leaf, std::size_t* value, bool write) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/node/node%d/hugepages/hugepages-2048kB/%s",
                  node(), leaf);
    std::FILE* f = std::fopen(path, write ? "w" : "r");
    if (f == nullptr) return false;
    const bool ok = write ? std::fprintf(f, "%zu", *value) > 0
                          : std::fscanf(f, "%zu", value) == 1;
    std::fclose(f);
    return ok;
}

[[nodiscard]] inline std::size_t pool_size() {
    std::size_t nr = 0;
    return pool("nr_hugepages", &nr, false) ? nr : 0;
}

inline std::size_t& borrowed() noexcept {
    static std::size_t n = 0;
    return n;
}

inline void give_back() {
    if (borrowed() == 0) return;
    std::size_t nr = 0;
    if (pool("nr_hugepages", &nr, false)) {
        std::size_t left = nr > borrowed() ? nr - borrowed() : 0;
        pool("nr_hugepages", &left, true);
    }
    borrowed() = 0;
}

[[nodiscard]] inline bool reserve(std::size_t pages) {
    if (node() < 0) bind_to(node_of_cpu(::sched_getcpu()));
    if (node() < 0) return false;
    std::size_t nr = 0, free_now = 0;
    if (!pool("nr_hugepages", &nr, false)) return false;
    if (!pool("free_hugepages", &free_now, false)) return false;
    if (borrowed() == 0 && nr > 0 && free_now == nr) {
        std::size_t zero = 0;
        pool("nr_hugepages", &zero, true);
        nr = free_now = 0;
    }
    if (free_now < pages) {
        std::size_t want = nr + (pages - free_now);
        if (!pool("nr_hugepages", &want, true)) return false;
        borrowed() += pages - free_now;
        if (!pool("free_hugepages", &free_now, false) || free_now < pages) {
            give_back();
            return false;
        }
    }
    static bool armed = false;
    if (!armed) {
        std::atexit(give_back);
        armed = true;
    }
    return true;
}

[[nodiscard]] inline void* map(std::size_t* bytes) {
    *bytes = (*bytes + kPage - 1) / kPage * kPage;
    if (*bytes == 0) return nullptr;
    void* m = MAP_FAILED;
    if (reserve(*bytes / kPage)) {
        m = ::mmap(nullptr, *bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);
    }
    if (m == MAP_FAILED) {
        static bool said = false;
        if (!said) {
            std::fprintf(stderr, "2 MB pages refused, falling back to 4 KB pages\n");
            said = true;
        }
        m = ::mmap(nullptr, *bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (m == MAP_FAILED) return nullptr;
    if (node() >= 0) {
        const unsigned long mask = 1ul << node();
        if (::syscall(SYS_mbind, m, *bytes, 2, &mask, 8 * sizeof(mask), 1u) != 0) {
            std::fprintf(stderr, "cannot tie %zu MB to node %d\n", *bytes >> 20, node());
        }
    }
    return m;
}

[[nodiscard]] inline int stray_page(const void* p, std::size_t bytes) {
    if (node() < 0 || p == nullptr || bytes == 0) return -1;
    const std::size_t n = bytes / kPage;
    for (std::size_t i = 0; i < n; ++i) {
        void* addr = const_cast<char*>(static_cast<const char*>(p)) + i * kPage;
        int where = -1;
        if (::syscall(SYS_move_pages, 0, 1, &addr, nullptr, &where, 0) != 0) return -1;
        if (where >= 0 && where != node()) return where;
    }
    return -1;
}

inline void must_be_local(const void* p, std::size_t bytes, const char* what) {
    const int stray = stray_page(p, bytes);
    if (stray < 0) return;
    std::fprintf(stderr, "%s: %zu MB landed on node %d, wanted node %d\n", what,
                 bytes >> 20, stray, node());
    std::abort();
}

template <typename T>
class Buffer {
public:
    Buffer() = default;
    Buffer(Buffer&& o) noexcept { swap(o); }
    Buffer& operator=(Buffer&& o) noexcept {
        swap(o);
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() {
        if (p_ != nullptr) ::munmap(p_, bytes_);
    }

    void assign(std::size_t n, const T& v) {
        take(n);
        for (std::size_t i = 0; i < n; ++i) new (p_ + i) T(v);
        must_be_local(p_, bytes_, "book");
    }
    void resize(std::size_t n) {
        take(n);
        for (std::size_t i = 0; i < n; ++i) new (p_ + i) T();
        must_be_local(p_, bytes_, "book");
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    T& back() noexcept { return p_[n_ - 1]; }
    T& operator[](std::size_t i) noexcept { return p_[i]; }
    const T& operator[](std::size_t i) const noexcept { return p_[i]; }

private:
    void swap(Buffer& o) noexcept {
        std::swap(p_, o.p_);
        std::swap(bytes_, o.bytes_);
        std::swap(n_, o.n_);
    }

    void take(std::size_t n) {
        if (p_ != nullptr) ::munmap(p_, bytes_);
        p_ = nullptr;
        n_ = n;
        bytes_ = n * sizeof(T);
        if (bytes_ == 0) return;
        void* m = map(&bytes_);
        if (m == nullptr) throw std::bad_alloc();
        p_ = static_cast<T*>(m);
    }

    T* p_ = nullptr;
    std::size_t bytes_ = 0, n_ = 0;
};

}
