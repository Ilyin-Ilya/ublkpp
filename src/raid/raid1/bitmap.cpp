#include "bitmap.hpp"

#include <bit>
#include <isa-l/mem_routines.h>
#include <ublk_cmd.h>

#include "raid1_superblock.hpp"
#include "lib/logging.hpp"

namespace ublkpp::raid1 {
constexpr auto bits_in_word = k_bits_in_byte * sizeof(Bitmap::word_t);

struct free_page {
    void operator()(void* x) { free(x); }
};

size_t Bitmap::max_pages_per_tx(const ublk_disk& device) { return device.max_tx() / k_page_size; }

Bitmap::Bitmap(uint64_t data_size, uint32_t chunk_size, uint32_t align, uint8_t* superbitmap_reserved,
               std::string const& id) :
        _id(id),
        _data_size(data_size),
        _chunk_size(chunk_size),
        _align(align),
        _page_width(_chunk_size * k_page_size * k_bits_in_byte),
        _num_pages(_data_size / _page_width + ((0 == _data_size % _page_width) ? 0 : 1)),
        _super_bitmap(superbitmap_reserved) {
    if (_num_pages > k_superbitmap_bits) {
        auto const max_capacity = k_superbitmap_bits * _page_width;
        throw std::runtime_error(fmt::format(
            "Device capacity {} exceeds SuperBitmap max capacity of {} pages (~31.4TB with 32KiB chunks, max {})",
            _data_size, k_superbitmap_bits, max_capacity));
    }
    RLOGT("Initializing RAID-1 BITMAP [pgs:{}, sz:{}Ki, id:{}]", _num_pages, _num_pages * k_page_size / Ki, _id)
    _page_map.resize(_num_pages);
    void* new_page{nullptr};
    if (auto err = ::posix_memalign(&new_page, _align, k_page_size); err)
        throw std::runtime_error("OutOfMemory"); // LCOV_EXCL_LINE
    memset(new_page, 0, k_page_size);
    _clean_page.reset(reinterpret_cast< word_t* >(new_page), free_page());
}

// We use uint64_t pointers to access the allocated pages. calc_bitmap_region will return:
//      * page_offset  : The page key in our page map (generated if we have a hole currently)
//      * word_offset  : The uint64_t offset within the raid1::page_size byte array
//      * shift_offset : The bits to begin setting within the word indicated
//      * nr_bits      : The number of bits fit in this word
//      * sz           : The number of bytes to represent as "dirty" from this index
std::tuple< uint32_t, uint32_t, uint32_t, uint32_t, uint64_t >
Bitmap::calc_bitmap_region(uint64_t addr, uint64_t len, uint32_t chunk_size) noexcept {
    // Precompute page_width_bits if constants are known
    auto const page_width_bits = chunk_size * k_page_size * k_bits_in_byte; // Compile-time if possible

    // Compute page and offset in one division
    auto const page = addr / page_width_bits;
    auto const page_off = addr % page_width_bits;

    // Compute bit position and size
    auto const page_bit = page_off / chunk_size;
    auto const sz = std::min(len, page_width_bits - page_off);

    // Compute number of bits (chunks) spanned
    auto const end_bit = (page_off + sz + chunk_size - 1) / chunk_size;
    auto const nr_bits = static_cast< uint32_t >(end_bit - (page_off / chunk_size));

    // Compute word and shift
    auto const word = page_bit / bits_in_word;
    auto const shift = bits_in_word - (page_bit % bits_in_word) - 1;

    return std::make_tuple(static_cast< uint32_t >(page), // Page index
                           word,                          // Word in the page
                           shift,                         // Shift within the word
                           nr_bits,                       // Number of bits
                           sz                             // Size of the region
    );
}

void Bitmap::init_to(std::shared_ptr< ublk_disk > device) {
    // Skip if the device passed to us is a missing-leg placeholder. Missing disks will never
    // succeed WRITEs and *must* be swapped with another disk for the RAID1 to begin resync.
    // Check before clear_all() so a missing device does not wipe superbitmap bits loaded from
    // the live device's on-disk superblock.
    if (device->is_missing()) [[unlikely]] {
        RLOGD("Device is MISSING, skipping init_to")
        return;
    }
    // Clear the SuperBitmap only when actually initializing a real device.
    _super_bitmap.clear_all();

    auto proto = iovec{.iov_base = _clean_page.get(), .iov_len = k_page_size};

    // TODO should be able to use discard if supported here. Need to add support in the Drivers first in sync_iov call
    // For now, create a scatter-gather of the MaxI/O size to clear synchronously erase the bitmap region.
    RLOGI("Clearing RAID-1 BITMAP [pgs:{}, sz:{}Ki, id:{}] on: {}", _num_pages, _num_pages * k_page_size / Ki, _id,
          *device)
    auto const max_pages = max_pages_per_tx(*device);
    if (0 == max_pages)
        throw std::runtime_error(
            fmt::format("Device max_tx()={} is smaller than bitmap page size ({})", device->max_tx(), k_page_size));
    auto iov = std::unique_ptr< iovec[] >(new iovec[max_pages]);
    if (!iov) throw std::runtime_error("OutOfMemory"); // LCOV_EXCL_LINE
    std::fill_n(iov.get(), max_pages, proto);

    auto const bitmap_start = k_page_size; // offset 0 is the superblock; bitmap pages follow immediately after
    for (auto pg_idx = 0UL; _num_pages > pg_idx;) {
        auto res = device->sync_iov(UBLK_IO_OP_WRITE, iov.get(), std::min(_num_pages - pg_idx, max_pages),
                                    bitmap_start + (pg_idx * k_page_size));
        if (!res) { throw std::runtime_error(fmt::format("Failed to write: {}", res.error().message())); }
        pg_idx += max_pages;
    }
}

io_result Bitmap::sync_to(ublk_disk& device, uint64_t offset) {
    // Allocate iovec array for batching consecutive pages
    auto const max_batch = max_pages_per_tx(device);
    if (0 == max_batch) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    auto iovs = std::unique_ptr< iovec[] >(new iovec[max_batch]);
    if (!iovs) return std::unexpected(std::make_error_condition(std::errc::not_enough_memory)); // LCOV_EXCL_LINE

    size_t iov_cnt = 0;
    uint32_t batch_start = 0;
    uint64_t batch_addr = 0;

    auto flush = [&]() -> io_result {
        if (0 == iov_cnt) return 0;
        RLOGD("Syncing {} consecutive Bitmap page(s) from page {} to {} [id: {}]", iov_cnt, batch_start, device, _id)
        auto res = device.sync_iov(UBLK_IO_OP_WRITE, iovs.get(), iov_cnt, batch_addr);
        iov_cnt = 0;
        return res;
    };

    for (auto pg_off = _super_bitmap.next_set_bit(0); pg_off < _num_pages;
         pg_off = _super_bitmap.next_set_bit(pg_off + 1)) {
        auto& page_data = _page_map[pg_off];

        // Pages loaded from disk that haven't been modified already match on-disk content — skip.
        if (page_data.loaded_from_disk.load(std::memory_order_acquire)) continue;

        auto page = page_data.page.load(std::memory_order_acquire);
        DEBUG_ASSERT(page, "SuperBitmap invariant violated: bit {} set but page is null", pg_off);
        if (!page) {
            RLOGW("SuperBitmap invariant violated: bit {} set but page is null", pg_off)
            continue;
        }

        bool consecutive = (iov_cnt > 0) && (pg_off == batch_start + iov_cnt);
        if (iov_cnt >= max_batch || (iov_cnt > 0 && !consecutive)) {
            if (auto res = flush(); !res) return res;
        }

        if (0 == iov_cnt) {
            batch_start = pg_off;
            batch_addr = (k_page_size * pg_off) + offset;
        }

        iovs[iov_cnt++] = {.iov_base = static_cast< void* >(page), .iov_len = k_page_size};
    }

    // Flush remaining bitmap pages
    if (auto res = flush(); !res) return res;

    // Note: SuperBitmap is saved together with SuperBlock by the caller
    // using write_superblock() which writes the entire 4KiB SuperBlock (including SuperBitmap)

    return 0;
}

bool Bitmap::superbitmap_nonempty() const noexcept { return _super_bitmap.next_set_bit(0) < _num_pages; }

void Bitmap::load_from(ublk_disk& device) {
    // Note: SuperBitmap must be loaded from SuperBlock BEFORE calling this function.
    // load_from is called during single-threaded init — no concurrent access, no lock needed.

    auto iov = iovec{.iov_base = nullptr, .iov_len = k_page_size};
    size_t pages_skipped = 0;
    for (auto pg_idx = 0UL; _num_pages > pg_idx; ++pg_idx) {
        // Check superbitmap first - skip loading if page is marked clean
        if (!_super_bitmap.test_bit(pg_idx)) {
            RLOGT("Skipping clean page: {} of {} page(s) [id: {}]", pg_idx + 1, _num_pages, _id)
            ++pages_skipped;
            continue;
        }

        RLOGT("Loading page: {} of {} page(s) [id: {}]", pg_idx + 1, _num_pages, _id)
        if (nullptr == iov.iov_base) {
            if (auto err = ::posix_memalign(&iov.iov_base, device.block_size(), k_page_size);
                0 != err || nullptr == iov.iov_base) [[unlikely]] { // LCOV_EXCL_START
                if (EINVAL == err) RLOGE("Invalid Argument while initializing superblock!")
                throw std::runtime_error("OutOfMemory");
            } // LCOV_EXCL_STOP
        }
        if (auto res = device.sync_iov(UBLK_IO_OP_READ, &iov, 1, k_page_size + (pg_idx * k_page_size)); !res) {
            free(iov.iov_base);
            throw std::runtime_error(fmt::format("Failed to read: {}", res.error().message()));
        }
        // If page is empty, clear any stale superbitmap bit and leave the slot unallocated.
        // This can happen after a crash where the page was cleaned but the on-disk superbitmap
        // was not updated. Clearing it here restores the invariant: bit set ↔ page non-null.
        if (0 == isal_zero_detect(iov.iov_base, k_page_size)) {
            _super_bitmap.clear_bit(pg_idx);
            continue;
        }
        RLOGT("Page: {} is *DIRTY* [id: {}]", pg_idx + 1, _id)
        _dirty_chunks_est += (k_page_size * k_bits_in_byte);

        // Store directly into the pre-existing slot (mark as loaded from disk, not modified)
        _page_map[pg_idx]._page_mem = iov.iov_base;
        _page_map[pg_idx].page.store(reinterpret_cast< word_t* >(iov.iov_base), std::memory_order_relaxed);
        _page_map[pg_idx].loaded_from_disk.store(true, std::memory_order_relaxed);
        iov.iov_base = nullptr;
    }
    if (nullptr != iov.iov_base) free(iov.iov_base);
    if (pages_skipped > 0) {
        RLOGI("Superbitmap optimization: skipped loading {} clean page(s) [id: {}]", pages_skipped, _id)
    }
}

Bitmap::PageData* Bitmap::__get_or_create_page(uint64_t offset) {
    auto& pd = _page_map[offset];

    // Fast path: page already allocated (lock-free load on word_t*)
    if (pd.page.load(std::memory_order_acquire)) return &pd;

    // Slow path: lazily allocate page memory and CAS it in (lock-free CAS on word_t*).
    // Two concurrent threads may both reach here; only one wins the CAS, the other discards its allocation.
    void* new_mem{nullptr};
    if (auto err = ::posix_memalign(&new_mem, _align, k_page_size); err)
        throw std::runtime_error("OutOfMemory"); // LCOV_EXCL_LINE
    memset(new_mem, 0, k_page_size);
    auto* new_page = reinterpret_cast< word_t* >(new_mem);

    word_t* expected = nullptr;
    if (pd.page.compare_exchange_strong(expected, new_page, std::memory_order_release, std::memory_order_acquire)) {
        pd._page_mem = new_mem; // We won — record ownership for cleanup at destruction
    } else {
        free(new_mem); // We lost — free immediately; pd.page is now valid from the winner
    }
    return &pd;
}

bool Bitmap::is_dirty(uint64_t addr, uint32_t len) noexcept {
    for (auto off = 0U; len > off;) {
        auto [page_offset, word_offset, shift_offset, nr_bits, sz] =
            calc_bitmap_region(addr + off, len - off, _chunk_size);
        off += sz;
        if (!_super_bitmap.test_bit(page_offset)) continue;
        auto page = _page_map[page_offset].page.load(std::memory_order_acquire);
        if (!page) continue;
        auto cur_word = page + word_offset;

        // Handle update crossing multiple words (optimization potential?)
        for (auto bits_left = nr_bits; 0 < bits_left;) {
            auto const bits_to_read = std::min(shift_offset + 1, bits_left);
            auto const bits_to_check = htobe64(64 == bits_to_read ? UINT64_MAX
                                                                  : (((uint64_t)0b1 << bits_to_read) - 1)
                                                       << (shift_offset - (bits_to_read - 1)));
            bits_left -= bits_to_read;
            if (0 != (std::atomic_ref< word_t >(*cur_word).load(std::memory_order_relaxed) & bits_to_check))
                return true;
            ++cur_word;
            shift_offset = bits_in_word - 1; // Word offset back to the beginning
        }
    }
    return false;
}

uint64_t Bitmap::page_size() noexcept { return k_page_size; }

size_t Bitmap::dirty_pages() noexcept {
    size_t dirty_cnt = 0;

    // Use SuperBitmap to skip pages known to be clean — avoids isal_zero_detect on every page.
    for (auto pg_off = _super_bitmap.next_set_bit(0); pg_off < _num_pages;
         pg_off = _super_bitmap.next_set_bit(pg_off + 1)) {
        auto& pd = _page_map[pg_off];
        auto page = pd.page.load(std::memory_order_relaxed);
        DEBUG_ASSERT(page, "SuperBitmap invariant violated: bit {} set but page is null", pg_off);
        if (!page) {
            RLOGW("SuperBitmap invariant violated: bit {} set but page is null", pg_off)
            continue;
        }

        ++dirty_cnt;
    }

    auto const full = (dirty_cnt * (k_page_size * k_bits_in_byte));
    if (full < _dirty_chunks_est.load(std::memory_order_relaxed))
        _dirty_chunks_est.store(full, std::memory_order_relaxed);
    return dirty_cnt;
}

std::tuple< Bitmap::word_t*, uint32_t, uint32_t > Bitmap::clean_region(uint64_t addr, uint32_t len) noexcept {
    auto [page_offset, word_offset, shift_offset, nr_bits, sz] = calc_bitmap_region(addr, len, _chunk_size);

    // Address and Length should be chunk aligned!
    DEBUG_ASSERT_EQ(0, addr % _chunk_size, "Address [addr:{:#0x}] is not aligned to {:#0x}", addr, _chunk_size)
    DEBUG_ASSERT_EQ(0, len % _chunk_size, "Len [len:{:#0x}] is not aligned to {:#0x}", len, _chunk_size)

    auto& page_data = _page_map[page_offset];
    auto page = page_data.page.load(std::memory_order_acquire);
    if (!page) {
        RLOGW("clean_region: page {} not found (already clean or cleared during device swap) [addr:{:#0x}, id: {}]",
              page_offset, addr, _id);
        return std::make_tuple(nullptr, page_offset, sz);
    }

    auto cur_word = page + word_offset;

    // Handle update crossing multiple words (optimization potential?)
    for (auto bits_left = nr_bits; 0 < bits_left;) {
        auto const bits_to_write = std::min(shift_offset + 1, bits_left);
        bits_left -= bits_to_write;
        auto const clear_mask = ~htobe64(64 == bits_to_write ? UINT64_MAX
                                                             : (((uint64_t)0b1 << bits_to_write) - 1)
                                                 << (shift_offset - (bits_to_write - 1)));
        auto old_word = std::atomic_ref< word_t >(*cur_word).fetch_and(clear_mask, std::memory_order_relaxed);
        _dirty_chunks_est.fetch_sub(
            std::min(_dirty_chunks_est.load(std::memory_order_relaxed),
                     static_cast< uint64_t >(std::popcount(old_word xor (old_word & clear_mask)))),
            std::memory_order_relaxed);
        ++cur_word;
        shift_offset = bits_in_word - 1; // Word offset back to the beginning
    }

    // Mark as modified AFTER all modifications (release ensures visibility)
    page_data.loaded_from_disk.store(false, std::memory_order_release);
    RLOGT("Bitmap CLEANED [addr:{:#0x}, len:{}KiB, dirty:{}KiB, id: {}]", addr, len / Ki, dirty_data_est() / Ki, _id)

    // Check if page became completely clean, and update superbitmap if so
    if (0 == isal_zero_detect(page, k_page_size)) {
        // Clear superbitmap bit for this now-clean page
        _super_bitmap.clear_bit(page_offset);
        return std::make_tuple(_clean_page.get(), page_offset, sz);
    }
    return std::make_tuple(nullptr, page_offset, sz);
}

uint64_t Bitmap::dirty_data_est() const noexcept {
    return _dirty_chunks_est.load(std::memory_order_relaxed) * _chunk_size;
}

// Scan for the first dirty chunk at or after min_lba. With min_lba=0 this is equivalent to
// the old next_dirty(): scans from page 0 with no bit masking. Returns {0, 0} if no dirty
// run exists at or after min_lba.
std::pair< uint64_t, uint32_t > Bitmap::next_dirty_after(uint64_t min_lba) noexcept {
    uint32_t const min_pg = static_cast< uint32_t >(min_lba / _page_width);

    for (auto pg_off = _super_bitmap.next_set_bit(min_pg); pg_off < _num_pages;
         pg_off = _super_bitmap.next_set_bit(pg_off + 1)) {
        uint32_t sz = 0;
        auto page = _page_map[pg_off].page.load(std::memory_order_acquire);
        DEBUG_ASSERT(page, "SuperBitmap invariant violated: bit {} set but page is null", pg_off);
        if (!page) {
            RLOGW("SuperBitmap invariant violated: bit {} set but page is null", pg_off)
            continue;
        }

        uint64_t const page_base = static_cast< uint64_t >(_page_width) * pg_off;

        // Within min_pg: skip words and bits that correspond to chunks before min_lba.
        // For subsequent pages the full page is scanned (word_start=0, bit_start=0).
        uint64_t const offset_in_page = (pg_off == min_pg && min_lba > page_base) ? (min_lba - page_base) : 0;
        uint32_t const min_chunk_in_page = static_cast< uint32_t >(offset_in_page / _chunk_size);
        uint32_t const word_start = min_chunk_in_page / bits_in_word;
        uint32_t const bit_start = min_chunk_in_page % bits_in_word;

        uint64_t logical_off = 0; // set inside the loop when a non-empty word is found
        uint64_t word = 0;
        for (auto word_off = word_start; (k_page_size / sizeof(word_t)) > word_off; ++word_off) {
            word = be64toh(std::atomic_ref< word_t >(*(page + word_off)).load(std::memory_order_relaxed));
            // Mask out bits (big-endian: bit 0 = MSB = smallest LBA in word) that fall before min_lba.
            if (word_off == word_start && bit_start > 0) word &= (UINT64_MAX >> bit_start);
            if (0 == word) continue;

            logical_off = page_base + (word_off * bits_in_word * _chunk_size);
            auto set_bit = std::countl_zero(word);
            logical_off += set_bit * _chunk_size;
            while ((static_cast< int >(bits_in_word) > set_bit) && ((word >> (bits_in_word - (set_bit++) - 1)) & 0b1)) {
                sz += _chunk_size;
            }
            break;
        }
        if (sz > 0) {
            if (_data_size < (logical_off + sz)) sz = static_cast< uint32_t >(_data_size - logical_off);
            return std::make_pair(logical_off, sz);
        }
    }
    return std::make_pair(0ULL, 0U);
}

// Returns:
//      * page         : Pointer to the page
//      * page_offset  : Page index
//      * sz           : The number of bytes from the provided `len` that fit in this page
void Bitmap::dirty_region(uint64_t addr, uint64_t len) {

    auto const end = addr + len;
    auto cur_off = addr;
    while (end > cur_off) {
        // Since we can require updating multiple pages on a page boundary write we need to loop here with a cursor
        // Calculate the tuple mentioned above
        auto [page_offset, word_offset, shift_offset, nr_bits, sz] =
            calc_bitmap_region(cur_off, end - cur_off, _chunk_size);
        cur_off += sz;

        auto page_data = __get_or_create_page(page_offset);
        if (!page_data) throw std::runtime_error("Could not insert new page"); // LCOV_EXCL_LINE

        // Clear loaded_from_disk BEFORE writing bits so that a concurrent sync_to cannot
        // observe loaded_from_disk=true after new dirty bits have already been ORed in.
        // If sync_to reads loaded_from_disk=false it will include this page in the flush,
        // which is correct whether or not we have finished setting bits yet (extra flush is safe).
        // The reverse ordering (bits first, then clear flag) is the TOCTOU bug: sync_to could
        // read loaded_from_disk=true, skip the page, and a subsequent crash would lose those bits.
        page_data->loaded_from_disk.store(false, std::memory_order_release);

        auto page = page_data->page.load(std::memory_order_acquire);
        auto cur_word = page + word_offset;
        // Handle update crossing multiple words (optimization potential?)
        for (auto bits_left = nr_bits; 0 < bits_left;) {
            auto const bits_to_write = std::min(shift_offset + 1, bits_left);
            auto const bits_to_set = htobe64(64 == bits_to_write ? UINT64_MAX
                                                                 : (((uint64_t)0b1 << bits_to_write) - 1)
                                                     << (shift_offset - (bits_to_write - 1)));
            bits_left -= bits_to_write;
            auto old_word = std::atomic_ref< word_t >(*cur_word).fetch_or(bits_to_set, std::memory_order_relaxed);
            _dirty_chunks_est.fetch_add(std::popcount(old_word xor (old_word | bits_to_set)),
                                        std::memory_order_relaxed);
            ++cur_word;
            shift_offset = bits_in_word - 1; // Word offset back to the beginning
        }

        // Update superbitmap to mark this page as dirty
        _super_bitmap.set_bit(page_offset);
    }
    RLOGT("Bitmap DIRTIED [addr:{:#0x}, len:{}KiB, dirty:{}KiB, id: {}]", addr, len / Ki, dirty_data_est() / Ki, _id)
}
} // namespace ublkpp::raid1
