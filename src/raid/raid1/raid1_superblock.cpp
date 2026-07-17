#include "raid1_superblock.hpp"

#include <boost/uuid/uuid_io.hpp>

#include "lib/logging.hpp"
#include "raid/superblock.hpp"

namespace ublkpp::raid1 {

auto format_as(SuperBlock const& sb) {
    auto read_uuid = boost::uuids::uuid();
    memcpy(read_uuid.data, sb.header.uuid, sizeof(sb.header.uuid));
    return fmt::format("[uuid:{}, ver:{:#0x}, age:{}, chunk_sz:{}Ki, read_route:{} (Side-{}:{}), resync:{}]",
                       to_string(read_uuid), be16toh(sb.header.version), be64toh(sb.fields.bitmap.age),
                       be32toh(sb.fields.bitmap.chunk_size) / Ki, static_cast< read_route >(sb.fields.read_route),
                       sb.fields.device_b ? "B" : "A", sb.fields.clean_unmount ? "Clean" : "Active",
                       static_cast< resync_copy_mode >(sb.fields.bitmap.resync_mode));
}

raid1::SuperBlock* pick_superblock(raid1::SuperBlock* dev_a, raid1::SuperBlock* dev_b) {
    // If either superblock is null, take the other
    if (!dev_a || !dev_b) return dev_a ? dev_a : dev_b;

    if (be64toh(dev_a->fields.bitmap.age) < be64toh(dev_b->fields.bitmap.age)) {
        dev_b->fields.read_route = static_cast< uint8_t >(read_route::DEVB);
        return dev_b;
    } else if (be64toh(dev_a->fields.bitmap.age) > be64toh(dev_b->fields.bitmap.age)) {
        dev_a->fields.read_route = static_cast< uint8_t >(read_route::DEVA);
        return dev_a;
    } else if (dev_a->fields.clean_unmount != dev_b->fields.clean_unmount) {
        // Ages equal, clean_unmount differs. A *healthy* (route==EITHER) array only gets here via an
        // asymmetric shutdown write (clean_unmount=1 landed on one leg only); equal ages then mean
        // the legs are identical, so return the clean one and keep the on-disk route -- nothing to
        // resync. For a *degraded* (route!=EITHER) array equal ages do NOT imply identical:
        // degradation and self-heal write BOTH superblocks while the destination still resyncs; that
        // divergence rides read_route + the superbitmap and is handled by
        // __init_bitmap_and_degraded_route, not here.
        //
        // NOTE FOR FUTURE ANALYSIS: do not add a read_route assignment here. Routing to the clean
        // device "to be safe" opens a healthy array degraded and runs a no-op resync on every
        // asymmetric shutdown.
        return dev_a->fields.clean_unmount ? dev_a : dev_b;
    }

    return dev_a;
}

static const uint8_t magic_bytes[16] = {0123, 045, 0377, 012, 064,  0231, 076, 0305,
                                        0147, 072, 0310, 027, 0111, 0256, 033, 0144};

static raid1::SuperBlock* read_superblock(ublk_disk& device) {
    auto const sb_size = sizeof(raid1::SuperBlock);
    DEBUG_ASSERT_EQ(0, sb_size % device.block_size(), "Device {} blocksize does not support alignment of [{}B]", device,
                    sb_size)
    auto iov = iovec{.iov_base = nullptr, .iov_len = sb_size};
    if (auto err = ::posix_memalign(&iov.iov_base, device.block_size(), sb_size); 0 != err || nullptr == iov.iov_base)
        [[unlikely]] { // LCOV_EXCL_START
        if (EINVAL == err) RLOGE("Invalid Argument while reading superblock!")
        RLOGE("Out of Memory while reading superblock!")
        return nullptr;
    } // LCOV_EXCL_STOP
    if (auto res = device.sync_iov(UBLK_IO_OP_READ, &iov, 1, 0UL); !res) {
        RLOGE("Could not read SuperBlock of [sz:{}] from: {} [res:{}]", sb_size, device, res.error().message())
        free(iov.iov_base);
        return nullptr;
    }
    return static_cast< raid1::SuperBlock* >(iov.iov_base);
}

io_result write_superblock(ublk_disk& device, raid1::SuperBlock const* sb, bool device_b, raid1::read_route read_route,
                           bool include_superbitmap, raid1::resync_copy_mode mode) {
    auto const sb_size = sizeof(raid1::SuperBlock);
    DEBUG_ASSERT_EQ(0, sb_size % device.block_size(), "Device {} blocksize does not support alignment of [{}B]", device,
                    sb_size)
    // Never mutate the shared SuperBlock in place. Build a stack-local write buffer so
    // concurrent write_superblock calls cannot race on the packed byte that clean_unmount,
    // read_route, and device_b share (M5).
    //
    // Live I/O path (include_superbitmap=false): copy only header + fields (74 bytes).
    // Leaving superbitmap_reserved zero avoids a TSan-visible non-atomic 8-byte load that
    // would overlap SuperBitmap::set_bit's concurrent atomic_ref::fetch_or at offset 74.
    //
    // Shutdown path (include_superbitmap=true): copy the full 4 KiB page so the on-disk
    // superbitmap is up-to-date. Safe because resync has been joined and I/O is quiesced
    // before the destructor runs; no concurrent set_bit/clear_bit can race the memcpy.
    alignas(4096) SuperBlock local{};
    if (include_superbitmap)
        memcpy(&local, sb, sb_size);
    else
        memcpy(&local, sb, offsetof(SuperBlock, superbitmap_reserved));
    local.fields.read_route = static_cast< uint8_t >(read_route);
    local.fields.device_b = device_b ? 1 : 0;
    local.fields.bitmap.resync_mode = static_cast< uint8_t >(mode);
    auto iov = iovec{.iov_base = &local, .iov_len = sb_size};
    auto res = device.sync_iov(UBLK_IO_OP_WRITE, &iov, 1, 0UL);
    RLOGI("Wrote: {} to: {}", local, device)
    if (!res) RLOGE("Error writing Superblock to: {}: {}", device, res.error().message())
    return res;
}

// Read and load the RAID1 superblock off a device. If it is not set, meaning the Magic is missing, then initialize
// the superblock to the current version. Existing disks are returned as-is; __init_params reconstructs the correct
// _reserved_size by branching on the version field.
std::expected< std::pair< raid1::SuperBlock*, bool >, std::error_condition >
load_superblock(ublk_disk& device, boost::uuids::uuid const& uuid, uint32_t const chunk_size) {
    auto sb = std::unique_ptr< raid1::SuperBlock, decltype(&free) >(read_superblock(device), free);
    if (!sb) return std::unexpected(std::make_error_condition(std::errc::io_error));
    bool was_new{false};
    if (memcmp(sb->header.magic, magic_bytes, sizeof(magic_bytes))) {
        memset(sb.get(), 0x00, raid1::k_page_size);
        memcpy(sb->header.magic, magic_bytes, sizeof(magic_bytes));
        memcpy(sb->header.uuid, uuid.data, sizeof(sb->header.uuid));
        sb->fields.clean_unmount = 1;
        sb->fields.bitmap.chunk_size = htobe32(chunk_size);
        sb->fields.bitmap.age = 0;
        sb->fields.read_route = static_cast< uint8_t >(read_route::EITHER);
        was_new = true;
        RLOGW("Missing superblock from: {}", device)
    } else
        RLOGI("Loaded: {} from: {}", *sb, device)

    // Verify some details in the superblock
    auto read_uuid = boost::uuids::uuid();
    memcpy(read_uuid.data, sb->header.uuid, sizeof(sb->header.uuid));
    if (uuid != read_uuid) {
        RLOGE("Superblock did not have a matching UUID expected: {} read: {}", to_string(uuid), to_string(read_uuid))
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (be16toh(sb->header.version) > k_sb_version) {
        RLOGE("Superblock version {:#0x} is newer than supported {:#0x} — refusing to open",
              be16toh(sb->header.version), k_sb_version)
        return std::unexpected(std::make_error_condition(std::errc::not_supported));
    }
    if (chunk_size != be32toh(sb->fields.bitmap.chunk_size)) {
        RLOGW("Superblock was created with different chunk_size: [{}B] will not use runtime config of [{}B] "
              "[uuid:{}] ",
              be32toh(sb->fields.bitmap.chunk_size), chunk_size, to_string(uuid))
    }

    if (was_new) {
        // Fresh device — stamp with current version.
        sb->header.version = htobe16(k_sb_version);
    }
    // Existing disks keep their on-disk version. __init_params branches on version to reconstruct
    // the original _reserved_size formula (v1: capacity-proportional, v2+: fixed k_superbitmap_bits).
    return std::make_pair(sb.release(), was_new);
}
} // namespace ublkpp::raid1
