#include "ublkpp/raid/raid0.hpp"

#include <boost/uuid/uuid_io.hpp>
#include <ublksrv.h>
#include <ublksrv_utils.h>

#include "raid0_impl.hpp"
#include "lib/logging.hpp"

namespace ublkpp {

class StripeDevice {
    struct destroy_sb {
        void operator()(raid0::SuperBlock* p) const {
            DEBUG_ASSERT_NOTNULL(p, "Freeing NULL ptr!") // LCOV_EXCL_LINE
            free(p);
        }
    };

public:
    StripeDevice(std::shared_ptr< UblkDisk > device, raid0::SuperBlock* super) :
            disk(std::move(device)), _sb(super, destroy_sb()) {}
    std::shared_ptr< UblkDisk > disk;
    std::unique_ptr< raid0::SuperBlock, destroy_sb > _sb;
};

static raid0::SuperBlock* read_superblock(UblkDisk& device);
static io_result write_superblock(UblkDisk& device, raid0::SuperBlock* sb);
static std::expected< raid0::SuperBlock*, std::error_condition >
load_superblock(UblkDisk& device, boost::uuids::uuid const& uuid, uint32_t& stripe_size, uint16_t const stripe_off);

Raid0Disk::Raid0Disk(boost::uuids::uuid const& uuid, uint32_t const stripe_size_bytes,
                     std::vector< std::shared_ptr< UblkDisk > >&& disks) :
        UblkDisk(), _stripe_size(stripe_size_bytes), _stride_width(_stripe_size * disks.size()) {
    // Discover overall Device parameters
    auto& our_params = *params();
    our_params.types |= UBLK_PARAM_TYPE_DISCARD;
    our_params.basic.dev_sectors = UINT64_MAX;
    direct_io = true;

    auto alt_stripe = false;
    our_params.basic.physical_bs_shift = ilog2(stripe_size_bytes);
    our_params.basic.io_opt_shift = ilog2(_stride_width);
    for (auto&& device : disks) {
        auto const& dev_params = *device->params();
        // We'll use dev_sectors to track the smallest array device we have
        our_params.basic.dev_sectors = std::min(our_params.basic.dev_sectors, dev_params.basic.dev_sectors);
        our_params.basic.logical_bs_shift =
            std::max(our_params.basic.logical_bs_shift, dev_params.basic.logical_bs_shift);
        our_params.basic.max_sectors = std::min(our_params.basic.max_sectors,
                                                static_cast< uint32_t >(dev_params.basic.max_sectors * disks.size()));

        if (!device->can_discard()) our_params.types &= ~UBLK_PARAM_TYPE_DISCARD;
        if (!device->uses_ublk_iouring) uses_ublk_iouring = false;

        direct_io = direct_io ? device->direct_io : false;

        auto this_alt_stripe = _stripe_size;
        auto sb = load_superblock(*device, uuid, this_alt_stripe, _stripe_array.size());
        if (_stripe_size != this_alt_stripe) {
            if (!alt_stripe) {
                alt_stripe = true;
                _stripe_size = this_alt_stripe;
            } else
                throw std::runtime_error(fmt::format("Could not read superblock! Mismatched Stripe Sizes!"));
        }
        if (!sb) throw std::runtime_error(fmt::format("Could not read superblock! {}", sb.error().message()));
        _stripe_array.emplace_back(std::make_unique< StripeDevice >(std::move(device), sb.value()));
    }

    // Finally we'll calculate the volume size as a multiple of the smallest array device
    // and adjust to account for the superblock we will write at the HEAD of each array device.
    // To keep things simple, we'll just use the first chunk from each device for ourselves.
    our_params.basic.dev_sectors -= (_stripe_size >> SECTOR_SHIFT);
    our_params.basic.dev_sectors *= _stripe_array.size();
    // Align size to max_sector size
    our_params.basic.dev_sectors -= (our_params.basic.dev_sectors % our_params.basic.max_sectors);

    if (can_discard())
        our_params.discard.discard_granularity = std::max(our_params.discard.discard_granularity, block_size());
}

Raid0Disk::~Raid0Disk() = default;

std::shared_ptr< UblkDisk > Raid0Disk::get_device(uint32_t stripe_offset) const {
    if (auto const width = _stripe_array.size(); width <= stripe_offset) {
        RLOGW("Stripe offset [{}] larger than array width [{}]", stripe_offset, width)
        return nullptr;
    }
    return _stripe_array[stripe_offset]->disk;
}

std::list< int > Raid0Disk::open_for_uring(int const iouring_device_start) {
    auto fds = std::list< int >();
    for (auto& stripe : _stripe_array) {
        fds.splice(fds.end(), stripe->disk->open_for_uring(iouring_device_start + fds.size()));
    }
    return fds;
}

io_result Raid0Disk::handle_internal(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                                     uint32_t nr_vecs, uint64_t addr, int res) {
    if (1 > nr_vecs) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    addr += _stride_width;
    return __distribute(
        iovecs, addr,
        [q, data, res, this](uint32_t stripe_off, sub_cmd_t new_sub_cmd, iovec* iov, uint32_t nr_iovs,
                             uint32_t logical_off) {
            return _stripe_array[stripe_off]->disk->handle_internal(q, data, new_sub_cmd, iov, nr_iovs, logical_off,
                                                                    res);
        },
        true, sub_cmd);
}

void Raid0Disk::idle_transition(ublksrv_queue const* q, bool enter) {
    for (auto const& stripe : _stripe_array) {
        stripe->disk->idle_transition(q, enter);
    }
}

void Raid0Disk::collect_async(ublksrv_queue const* q, std::list< async_result >& results) {
    for (auto const& stripe : _stripe_array) {
        if (!stripe->disk->uses_ublk_iouring) stripe->disk->collect_async(q, results);
    }
}

io_result Raid0Disk::handle_flush(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd) {
    bool const retry{is_retry(sub_cmd)};
    if (!retry) sub_cmd = shift_route(sub_cmd, route_size());
    auto cnt{0UL};
    auto stripe_off{0U};
    for (auto const& stripe : _stripe_array) {
        auto const new_sub_cmd = sub_cmd + (!retry ? stripe_off : 0U);
        auto res = stripe->disk->handle_flush(q, data, new_sub_cmd);
        if (!res) return res;
        cnt += res.value();
        ++stripe_off;
    }
    return cnt;
}

io_result Raid0Disk::handle_discard(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                                    uint64_t addr) {
    bool const retry{is_retry(sub_cmd)};
    if (!retry) sub_cmd = shift_route(sub_cmd, route_size());

    auto const route_mask = _max_stripe_cnt - 1;

    // Adjust the address for our superblock area, do not use _addr_ beyond this.
    auto const lba = addr >> params()->basic.logical_bs_shift;
    addr += _stride_width;

    auto cnt{0U};
    for (auto const& [stripe_off, region] : raid0::merged_subcmds(_stride_width, _stripe_size, addr, len)) {
        auto const& [logical_off, logical_len] = region;
        auto const& device = _stripe_array[stripe_off]->disk;
        if (retry && (stripe_off != ((sub_cmd >> device->route_size()) & route_mask))) [[unlikely]]
            continue;
        sub_cmd_t const new_sub_cmd = sub_cmd + (!retry ? stripe_off : 0);
        auto const logical_lba = logical_off >> params()->basic.logical_bs_shift;

        RLOGD("Received DISCARD: [tag:{:#0x}] ublk io [lba:{:#0x}|len:{:#0x}] -> "
              "[stripe_off:{}|logical_lba:{:#0x}|logical_len:{:#0x}|sub_cmd:{}]",
              data->tag, lba, len, stripe_off, logical_lba, logical_len, ublkpp::to_string(new_sub_cmd))
        auto res = device->handle_discard(q, data, new_sub_cmd, logical_len, logical_off);
        if (!res) return res;
        cnt += res.value();
    }
    return cnt;
}

/// This is the primary I/O handler call for RAID0
//
//  RAID0 is primary responsible for splitting an I/O request across several stripes. These operations can cross
//  stripe boundaries and even wrap around several strides. This routine handles this calculation and calls
//  the given routine `func` for each stripe that it has collected scatter (struct iovec) operations for.
io_result Raid0Disk::__distribute(iovec* iovecs, uint64_t addr, auto&& func, bool retry, sub_cmd_t sub_cmd) const {
    // Each thread has a 2-dimensional block of iovecs that we can split into
    thread_local auto sub_cmds =
        std::array< std::tuple< uint64_t, uint32_t, std::array< iovec, 16 > >, _max_stripe_cnt >();

    // Special case for single device
    if (1 == _stripe_array.size()) return func(0, sub_cmd, iovecs, 1, addr);

    auto const route_mask = _max_stripe_cnt - 1;

    DEBUG_ASSERT_LE(iovecs->iov_len, UINT32_MAX) // LCOV_EXCL_LINE
    auto const len = (uint32_t)iovecs->iov_len;
    uint32_t cnt{0};
    for (auto off = 0U; len > off;) {
        auto const [stripe_off, logical_off, sz] =
            raid0::next_subcmd(_stride_width, _stripe_size, addr + off, (len - off));

        // Ensure we advance here in case we _continue_ or anything later
        auto buf_cursor = (uint8_t*)iovecs->iov_base + off;
        off += sz;

        // Get the device
        auto const& device = _stripe_array[stripe_off]->disk;

        // If this is a retry, we only want to re-issue the operation whose route matches the one passed in
        if (retry) [[unlikely]] {
            // Mask off to get "our" portion of the original route and see if the device that processed this
            // operation matches the current RAID-0 sub-operation; if not then skip.
            if (stripe_off != ((sub_cmd >> device->route_size()) & route_mask)) continue;
        }

        auto& [io_addr, alive_cmds, io_array] = sub_cmds[stripe_off];
        { // Fillout iovec
            auto& iov = io_array[alive_cmds++];
            iov.iov_base = (void*)buf_cursor;
            iov.iov_len = sz;
        }
        if (1 == alive_cmds) [[likely]]
            io_addr = logical_off;

        // Last sub_cmd for this device, issue now
        if ((_stride_width - _stripe_size) >= (len - off)) {
            sub_cmd_t const new_sub_cmd = sub_cmd + (!retry ? (uint16_t)stripe_off : 0);
            DEBUG_ASSERT_LE(alive_cmds, UINT32_MAX) // LCOV_EXCL_LINE
            auto res = func(stripe_off, new_sub_cmd, io_array.data(), alive_cmds, io_addr);
            // Set this back to zero so the next command can reuse
            alive_cmds = 0;
            if (!res) return res;
            cnt += res.value();
        }
    }
    return cnt;
}

io_result Raid0Disk::async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                               uint32_t nr_vecs, uint64_t addr) {
    // RAID-0 only supports not-scattered I/O currently!
    if (1 > nr_vecs) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));

    bool const retry{is_retry(sub_cmd)};
    if (!retry) sub_cmd = shift_route(sub_cmd, route_size());
    auto const lba = addr >> params()->basic.logical_bs_shift;
    RLOGT("Received {}: [tag:{:#0x}] ublk io [lba:{:#0x}|len:{:#0x}] [sub_cmd:{}]",
          ublksrv_get_op(data->iod) == UBLK_IO_OP_READ ? "READ" : "WRITE", data->tag, lba, iovecs->iov_len,
          ublkpp::to_string(sub_cmd))

    // Adjust the address for our superblock area, do not use _addr_ beyond this.
    addr += _stride_width;

    return __distribute(
        iovecs, addr,
        [q, data, this](uint32_t stripe_off, sub_cmd_t new_sub_cmd, iovec* iov, uint32_t nr_iovs,
                        uint64_t logical_off) {
            auto const logical_lba = logical_off >> params()->basic.logical_bs_shift;
            RLOGT("Perform {}: [tag:{:#0x}] ublk aysnc_io -> "
                  "[stripe_off:{}|logical_lba:{:#0x}|logical_len:{:#0x}|sub_cmd:{}]",
                  ublksrv_get_op(data->iod) == UBLK_IO_OP_READ ? "READ" : "WRITE", data->tag, stripe_off, logical_lba,
                  __iovec_len(iov, iov + nr_iovs), ublkpp::to_string(new_sub_cmd))
            return _stripe_array[stripe_off]->disk->async_iov(q, data, new_sub_cmd, iov, nr_iovs, logical_off);
        },
        retry, sub_cmd);
}

io_result Raid0Disk::sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) noexcept {
    // RAID-0 only supports not-scattered I/O currently!
    if (1 > nr_vecs) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));

    // Adjust the address for our superblock area, do not use _addr_ beyond this.
    addr += _stride_width;

    return __distribute(iovecs, addr,
                        [op, this](uint32_t stripe_off, sub_cmd_t, iovec* iov, uint32_t nr_iovs, uint64_t logical_off) {
                            RLOGT("Perform {}: ublk sync_io -> "
                                  "[stripe_off:{}|logical_sector:{}|logical_len:{:#0x}]",
                                  op == UBLK_IO_OP_READ ? "READ" : "WRITE", stripe_off, logical_off >> SECTOR_SHIFT,
                                  __iovec_len(iov, iov + nr_iovs))
                            return _stripe_array[stripe_off]->disk->sync_iov(op, iov, nr_iovs, logical_off);
                        });
}

static const uint8_t magic_bytes[16] = {0127, 0345, 072,  0211, 0254, 033,  070,  0146,
                                        0125, 0377, 0204, 065,  0131, 0120, 0306, 047};
constexpr auto SB_VERSION = 1;

static raid0::SuperBlock* read_superblock(UblkDisk& device) {
    auto const sb_size = sizeof(raid0::SuperBlock);
    RLOGT("Reading Superblock from: [{}] {}%{} == {}", device, sb_size, device.block_size(),
          sb_size % device.block_size())
    DEBUG_ASSERT_EQ(0, sb_size % device.block_size(), "Device [{}] blocksize does not support alignment of [{}B]",
                    device, sb_size)
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
    return static_cast< raid0::SuperBlock* >(iov.iov_base);
}

static io_result write_superblock(UblkDisk& device, raid0::SuperBlock* sb) {
    auto const sb_size = sizeof(raid0::SuperBlock);
    RLOGT("Writing Superblock to: [{}]", device)
    DEBUG_ASSERT_EQ(0, sb_size % device.block_size(), "Device {} blocksize does not support alignment of [{}B]", device,
                    sb_size)
    auto iov = iovec{.iov_base = sb, .iov_len = sb_size};
    auto res = device.sync_iov(UBLK_IO_OP_WRITE, &iov, 1, 0UL);
    if (!res) RLOGE("Error writing Superblock to: [{}]!", device, res.error().message())
    return res;
}

// Read and load the RAID0 superblock off a device. If it is not set, meaning the Magic is missing, then initialize
// the superblock to the current version. Otherwise migrate any changes needed after version discovery.
static std::expected< raid0::SuperBlock*, std::error_condition >
load_superblock(UblkDisk& device, boost::uuids::uuid const& uuid, uint32_t& stripe_size, uint16_t const stripe_off) {
    auto sb = read_superblock(device);
    if (!sb) return std::unexpected(std::make_error_condition(std::errc::io_error));

    // Check for MAGIC, initialize SB if missing
    if (memcmp(sb->header.magic, magic_bytes, sizeof(magic_bytes))) {
        RLOGI("Initializing RAID-0 on {} [stripe_size:{}KiB, uuid:{}]", device, stripe_size / Ki, to_string(uuid))
        memset(sb, 0x00, sizeof(raid0::SuperBlock));
        memcpy(sb->header.magic, magic_bytes, sizeof(magic_bytes));
        memcpy(sb->header.uuid, uuid.data, sizeof(sb->header.uuid));
        sb->fields.stripe_off = htobe16(stripe_off);
        sb->fields.stripe_size = htobe32(stripe_size);
    }

    // Verify some details in the superblock
    auto read_uuid = boost::uuids::uuid();
    memcpy(read_uuid.data, sb->header.uuid, sizeof(sb->header.uuid));
    if (uuid != read_uuid) {
        RLOGE("Superblock did not have a matching UUID expected: {} read: {}", to_string(uuid), to_string(read_uuid))
        free(sb);
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    if (stripe_off != be16toh(sb->fields.stripe_off)) {
        RLOGE("Superblock does not match given array parameters: Expected [stripe_off:{}] != Found [stripe_off:{}]",
              stripe_off, be16toh(sb->fields.stripe_off))
        free(sb);
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
    auto const read_stripe_size = be32toh(sb->fields.stripe_size);
    if (stripe_size != read_stripe_size) {
        RLOGW(
            "Superblock does not match given array parameters: Expected [stripe_sz:{:#0x}] != Found [stripe_sz:{:#0x}]",
            stripe_size, read_stripe_size)
        stripe_size = read_stripe_size;
    }
    auto const sb_ver = be16toh(sb->header.version);
    RLOGI("Loaded v{:#0x} superblock [stripe_sz:{}Ki, stripe_off:{}, uuid:{}] from: {}", sb_ver, stripe_size / Ki,
          stripe_off, to_string(uuid), device)

    // Migrating to latest version
    if (SB_VERSION > sb_ver) {
        sb->header.version = htobe16(SB_VERSION);
        if (!write_superblock(device, sb)) {
            free(sb);
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        }
    }
    return sb;
}

void Raid0Disk::on_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) {
    // First, let the underlying device handle its portion of the routing
    // We need to determine which stripe handled this I/O by extracting our routing bits
    // from sub_cmd, accounting for the underlying device's routing bits.
    auto const route_mask = _max_stripe_cnt - 1;

    // Extract stripe index from sub_cmd (shift past underlying device's route bits)
    auto const stripe_idx = static_cast< size_t >((sub_cmd >> _stripe_array[0]->disk->route_size()) & route_mask);

    RLOGT("Raid0Disk::on_io_complete [tag:{:#0x}] [sub_cmd:{}] stripe_idx:{}", data->tag, ublkpp::to_string(sub_cmd), stripe_idx)

    // Pass completion notification to the underlying device for its metrics
    if (stripe_idx < _stripe_array.size()) {
        _stripe_array[stripe_idx]->disk->on_io_complete(data, sub_cmd);
    }
}

} // namespace ublkpp
