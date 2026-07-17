#pragma once

extern "C" {
#include <endian.h>
}

#include <boost/uuid/uuid.hpp>
#include <sisl/logging/logging.h>
#include <sisl/utility/enum.hpp>

#include "ublkpp/lib/ublk_disk.hpp"

#include "lib/common.hpp"

namespace ublkpp {

namespace raid1 {
constexpr auto const k_bits_in_byte = 8UL;
constexpr uint16_t k_sb_version = 2;
//  Cap some array parameters so we can make simple assumptions later
constexpr auto k_min_chunk_size = 32 * Ki;
// Use a single bit to represent each chunk
constexpr auto k_page_size = 4 * Ki;

/*
Was calculated as 4Kib - other superblock fields (with padding)
*/
constexpr size_t k_superbitmap_size = 4022;
constexpr size_t k_superbitmap_bits = k_superbitmap_size * k_bits_in_byte;

ENUM(read_route, uint8_t, EITHER = 0, DEVA = 1, DEVB = 2);

// How a resync copies each region to the destination leg; selected by Raid1Disk, applied per page
// by __copy_region (raid1_resync_task.cpp), and persisted in the superblock (bitmap.resync_mode) so
// a clean degraded restart resumes the same mode.
//   BLIND     - always write. A genuinely-fresh leg without assume_clean, and known-divergent
//               degraded-return dirty sets, where comparing adds no value.
//   CHECK     - read the destination and memcmp per page; write only divergent pages. The default
//               whenever the destination still holds data (self-heal, re-add of a leg with a
//               superblock). Always correct; only ever adds a destination read.
//   ZERO_TEST - zero-detect each source page and write only non-zero ones, never reading the
//               destination (thin-preserving). Only a genuinely uninitialized leg (no superblock)
//               whose caller asserted read-zero-when-unallocated (assume_clean).
ENUM(resync_copy_mode, uint8_t, BLIND = 0, CHECK = 1, ZERO_TEST = 2);

#ifdef __LITTLE_ENDIAN
struct __attribute__((__packed__)) SuperBlock {
    struct {
        uint8_t magic[16]; // 128-bit magic to detect an initialized superblock
        uint16_t version;
        uint8_t uuid[16]; // This is a user UUID that is assigned when the array is created
    } header;             // 34 bytes
    struct {
        // was cleanly unmounted, position in RAID1 and current Healthy device
        uint8_t clean_unmount : 1, read_route : 2, device_b : 1, : 0;
        struct {
            uint8_t resync_mode;   // Persisted resync_copy_mode (0=BLIND legacy default)
            uint8_t _reserved[15]; // Unused
            uint32_t chunk_size;   // Number of bytes each bit represents
            uint64_t age;
        } bitmap;
    } fields;                                         // 40 bytes (with padding)
    uint8_t superbitmap_reserved[k_superbitmap_size]; // Space for SuperBitmap (completes 4KiB page)
};
static_assert(k_page_size == sizeof(SuperBlock), "Size of raid1::SuperBlock does not match SIZE!");
static_assert(sizeof(SuperBlock::header) == 34, "SuperBlock::header size mismatch");
static_assert(sizeof(((SuperBlock*)nullptr)->fields) == 40, "SuperBlock::fields size mismatch");
static_assert(offsetof(SuperBlock, superbitmap_reserved) == 74, "SuperBlock::superbitmap_reserved offset mismatch");
// Pin the on-disk position of the resync_mode byte (carved from bitmap._reserved). If the bitmap
// sub-struct is reordered or repadded, an existing array would decode the mode from the wrong byte.
static_assert(offsetof(SuperBlock, fields.bitmap.resync_mode) == 42, "resync_mode byte moved; on-disk layout changed");
#else
#error "Big Endian not supported!"
#endif

auto format_as(SuperBlock const& sb);

extern SuperBlock* pick_superblock(SuperBlock* dev_a, raid1::SuperBlock* dev_b);
extern io_result write_superblock(ublk_disk& device, raid1::SuperBlock const* sb, bool device_b, read_route read_route,
                                  bool include_superbitmap = false, resync_copy_mode mode = resync_copy_mode::BLIND);
extern std::expected< std::pair< raid1::SuperBlock*, bool >, std::error_condition >
load_superblock(ublk_disk& device, boost::uuids::uuid const& uuid, uint32_t const chunk_size);

} // namespace raid1
} // namespace ublkpp
