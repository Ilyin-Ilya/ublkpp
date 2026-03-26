#include "raid1_superblock.hpp"

#include <boost/uuid/uuid_io.hpp>

#include "lib/logging.hpp"
#include "raid/superblock.hpp"

namespace ublkpp::raid1 {

auto format_as(SuperBlock const& sb) {
    auto read_uuid = boost::uuids::uuid();
    memcpy(read_uuid.data, sb.header.uuid, sizeof(sb.header.uuid));
    return fmt::format("[uuid:{}, ver:{:#0x}, age:{}, chunk_sz:{}Ki, read_route:{:#0x} (Side-{}:{})]",
                       to_string(read_uuid), be16toh(sb.header.version), be64toh(sb.fields.bitmap.age),
                       be32toh(sb.fields.bitmap.chunk_size) / Ki, sb.fields.read_route, sb.fields.device_b ? "B" : "A",
                       sb.fields.clean_unmount ? "Clean" : "Dirty");
}

raid1::SuperBlock* pick_superblock(raid1::SuperBlock* dev_a, raid1::SuperBlock* dev_b) {
    if (be64toh(dev_a->fields.bitmap.age) < be64toh(dev_b->fields.bitmap.age)) {
        dev_b->fields.read_route = static_cast< uint8_t >(read_route::DEVB);
        return dev_b;
    } else if (be64toh(dev_a->fields.bitmap.age) > be64toh(dev_b->fields.bitmap.age)) {
        dev_a->fields.read_route = static_cast< uint8_t >(read_route::DEVA);
        return dev_a;
    } else if (dev_a->fields.clean_unmount != dev_b->fields.clean_unmount)
        return dev_a->fields.clean_unmount ? dev_a : dev_b;

    return dev_a;
}

static const uint8_t magic_bytes[16] = {0123, 045, 0377, 012, 064,  0231, 076, 0305,
                                        0147, 072, 0310, 027, 0111, 0256, 033, 0144};

constexpr auto SB_VERSION = 1;

static raid1::SuperBlock* read_superblock(UblkDisk& device) {
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
        RLOGE("Could not read SuperBlock of [sz:{}] [res:{}]", sb_size, res.error().message())
        free(iov.iov_base);
        return nullptr;
    }
    return static_cast< raid1::SuperBlock* >(iov.iov_base);
}

io_result write_superblock(UblkDisk& device, raid1::SuperBlock* sb, bool device_b, raid1::read_route read_route) {
    auto const sb_size = sizeof(raid1::SuperBlock);
    DEBUG_ASSERT_EQ(0, sb_size % device.block_size(), "Device {} blocksize does not support alignment of [{}B]", device,
                    sb_size)
    sb->fields.read_route = static_cast<uint8_t>(read_route);
    if (device_b) sb->fields.device_b = 1;
    auto iov = iovec{.iov_base = sb, .iov_len = sb_size};
    auto res = device.sync_iov(UBLK_IO_OP_WRITE, &iov, 1, 0UL);
    RLOGI("Wrote: {} to: {}", *sb, device)
    sb->fields.device_b = 0;
    if (!res) RLOGE("Error writing Superblock to: {}: {}", device, res.error().message())
    return res;
}

// Read and load the RAID1 superblock off a device. If it is not set, meaning the Magic is missing, then initialize
// the superblock to the current version. Otherwise migrate any changes needed after version discovery.
std::expected< std::pair< raid1::SuperBlock*, bool >, std::error_condition >
load_superblock(UblkDisk& device, boost::uuids::uuid const& uuid, uint32_t const chunk_size) {
    auto sb = read_superblock(device);
    if (!sb) return std::unexpected(std::make_error_condition(std::errc::io_error));
    bool was_new{false};
    if (memcmp(sb->header.magic, magic_bytes, sizeof(magic_bytes))) {
        memset(sb, 0x00, raid1::k_page_size);
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
        free(sb);
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (chunk_size != be32toh(sb->fields.bitmap.chunk_size)) {
        RLOGW("Superblock was created with different chunk_size: [{}B] will not use runtime config of [{}B] "
              "[uuid:{}] ",
              be32toh(sb->fields.bitmap.chunk_size), chunk_size, to_string(uuid))
    }

    if (SB_VERSION > be16toh(sb->header.version)) { sb->header.version = htobe16(SB_VERSION); }
    return std::make_pair(sb, was_new);
}
} // namespace ublkpp::raid1
