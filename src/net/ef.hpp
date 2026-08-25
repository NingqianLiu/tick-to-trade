#pragma once

#include <sys/mman.h>

#include <etherfabric/base.h>
#include <etherfabric/memreg.h>
#include <etherfabric/pd.h>
#include <etherfabric/vi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "common/huge.hpp"
#include "common/settings.hpp"

namespace ef {

class Vi {
public:
    Vi() = default;
    Vi(const Vi&) = delete;
    Vi& operator=(const Vi&) = delete;

    ~Vi() {
        if (has_vi_) ef_vi_free(&vi_, dh_);
        if (has_pd_) ef_pd_free(&pd_, dh_);
        if (has_dh_) ef_driver_close(dh_);
    }

    [[nodiscard]] bool open(const char* intf, int rxq, int txq,
                            enum ef_vi_flags flags) {
        int rc = ef_driver_open(&dh_);
        if (rc < 0) return fail("ef_driver_open", rc);
        has_dh_ = true;
        rc = ef_pd_alloc_by_name(&pd_, dh_, intf, EF_PD_DEFAULT);
        if (rc < 0) return fail("ef_pd_alloc_by_name", rc);
        has_pd_ = true;
        rc = ef_vi_alloc_from_pd(&vi_, dh_, &pd_, dh_, -1, rxq, txq, nullptr, -1,
                                 flags);
        if (rc < 0) return fail("ef_vi_alloc_from_pd", rc);
        has_vi_ = true;
        return true;
    }

    [[nodiscard]] ef_vi* get() noexcept { return &vi_; }
    [[nodiscard]] ef_driver_handle dh() const noexcept { return dh_; }
    [[nodiscard]] ef_pd* pd() noexcept { return &pd_; }

private:
    static bool fail(const char* what, int rc) {
        std::fprintf(stderr, "%s: %d\n", what, rc);
        return false;
    }

    ef_driver_handle dh_{};
    ef_pd pd_{};
    ef_vi vi_{};
    bool has_dh_ = false, has_pd_ = false, has_vi_ = false;
};

class Frames {
public:
    Frames() = default;
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    ~Frames() {
        if (base_ != nullptr) ::munmap(base_, bytes_);
    }

    [[nodiscard]] bool alloc(Vi& vi, std::size_t slots, std::size_t slot_bytes) {
        slots_ = slots;
        slot_bytes_ = slot_bytes;
        bytes_ = slots * slot_bytes;
        void* p = huge::map(&bytes_);
        if (p == nullptr) {
            std::fprintf(stderr,
                         "cannot map %zu bytes of huge pages; reserve more with "
                         "sysctl vm.nr_hugepages\n",
                         bytes_);
            return false;
        }
        base_ = static_cast<std::uint8_t*>(p);
        for (std::size_t off = 0; off < bytes_; off += cfg::kHugePageBytes) {
            base_[off] = 0;
        }
        huge::must_be_local(base_, bytes_, "card buffers");
        const int rc = ef_memreg_alloc(&mr_, vi.dh(), vi.pd(), vi.dh(), base_, bytes_);
        if (rc < 0) {
            std::fprintf(stderr, "ef_memreg_alloc: %d\n", rc);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint8_t* at(std::size_t slot) noexcept {
        return base_ + slot * slot_bytes_;
    }
    [[nodiscard]] ef_addr dma(std::size_t slot) noexcept {
        return ef_memreg_dma_addr(&mr_, slot * slot_bytes_);
    }
    [[nodiscard]] std::size_t slots() const noexcept { return slots_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    std::uint8_t* base_ = nullptr;
    std::size_t bytes_ = 0, slots_ = 0, slot_bytes_ = 0;
    ef_memreg mr_{};
};

}
