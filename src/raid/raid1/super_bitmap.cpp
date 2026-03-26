#include "super_bitmap.hpp"

#include <atomic>
#include <cstring>

#include "lib/logging.hpp"

// SuperBitmap implementation uses lock-free atomic operations for thread safety.
// Individual bit operations (set_bit, clear_bit, test_bit) use std::atomic_ref
// to perform atomic read-modify-write operations on individual bytes.
// This allows concurrent modifications to different bits in the same byte without races.

namespace ublkpp::raid1 {

SuperBitmap::SuperBitmap(uint8_t* superblock_reserved_field)
    : _bits(superblock_reserved_field) {
    // NOTE: We do NOT clear_all() here because the superblock may contain
    // existing bitmap state that was loaded from disk. The caller should
    // explicitly call clear_all() if they want to initialize a new bitmap.
}

void SuperBitmap::set_bit(uint32_t page_idx) {
    DEBUG_ASSERT_LT(page_idx, k_superbitmap_bits, "SuperBitmap page_idx out of bounds");
    auto const byte_idx = page_idx / 8;
    auto const bit_idx = page_idx % 8;
    // Use atomic fetch_or to safely set the bit without racing with other bit operations
    std::atomic_ref<uint8_t>(_bits[byte_idx]).fetch_or(1U << bit_idx, std::memory_order_relaxed);
}

void SuperBitmap::clear_bit(uint32_t page_idx) {
    DEBUG_ASSERT_LT(page_idx, k_superbitmap_bits, "SuperBitmap page_idx out of bounds");
    auto const byte_idx = page_idx / 8;
    auto const bit_idx = page_idx % 8;
    // Use atomic fetch_and to safely clear the bit without racing with other bit operations
    std::atomic_ref<uint8_t>(_bits[byte_idx]).fetch_and(~(1U << bit_idx), std::memory_order_relaxed);
}

bool SuperBitmap::test_bit(uint32_t page_idx) const {
    DEBUG_ASSERT_LT(page_idx, k_superbitmap_bits, "SuperBitmap page_idx out of bounds");
    auto const byte_idx = page_idx / 8;
    auto const bit_idx = page_idx % 8;
    // Use atomic load to safely read the byte without seeing torn reads
    auto const byte_val = std::atomic_ref<const uint8_t>(_bits[byte_idx]).load(std::memory_order_relaxed);
    return (byte_val & (1U << bit_idx)) != 0;
}

void SuperBitmap::clear_all() {
    if (!_bits) return;
    // NOTE: clear_all() should only be called during initialization (init_to) when
    // no concurrent access is happening. Using memset here is safe in that context.
    // If concurrent access is possible, the caller must provide external synchronization.
    memset(_bits, 0x00, k_superbitmap_size);
}

uint8_t* SuperBitmap::data() {
    return _bits;
}

const uint8_t* SuperBitmap::data() const {
    return _bits;
}

} // namespace ublkpp::raid1
