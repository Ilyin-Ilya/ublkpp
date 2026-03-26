#pragma once

#include <cstddef>
#include <cstdint>

#include "raid1_superblock.hpp"

namespace ublkpp::raid1 {

class SuperBitmap {
private:
    uint8_t* const _bits;  // Const pointer to SuperBlock.superbitmap_reserved (cannot be reassigned, NOT owned)

public:
    // Constructor takes pointer to SuperBlock.superbitmap_reserved field
    SuperBitmap(uint8_t* superblock_reserved_field);

    // Set bit for a bitmap page (mark as dirty)
    void set_bit(uint32_t page_idx);

    // Clear bit for a bitmap page (mark as clean)
    void clear_bit(uint32_t page_idx);

    // Test if bitmap page is dirty
    bool test_bit(uint32_t page_idx) const;

    // Clear all bits (mark all pages clean)
    void clear_all();

    // Get raw data pointer (points into SuperBlock.superbitmap_reserved)
    uint8_t* data();
    const uint8_t* data() const;
};

} // namespace ublkpp::raid1
