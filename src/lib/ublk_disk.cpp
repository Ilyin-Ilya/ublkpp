#include "ublkpp/lib/ublk_disk.hpp"

#include <ublksrv.h>
#include <ublk_cmd.h>

#include "logging.hpp"

namespace ublkpp {

UblkDisk::UblkDisk() :
        _params(std::make_unique< ublk_params >(ublk_params{
            .len = 0,
            .types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DMA_ALIGN,
            .basic =
                {
                    .attrs = UBLK_ATTR_VOLATILE_CACHE | UBLK_ATTR_FUA,
                    .logical_bs_shift = DEFAULT_BS_SHIFT, // 4KiB by default (derived can override)
                    .physical_bs_shift = DEFAULT_BS_SHIFT,
                    .io_opt_shift = DEFAULT_BS_SHIFT,
                    .io_min_shift = DEFAULT_BS_SHIFT,
                    .max_sectors = DEF_BUF_SIZE >> SECTOR_SHIFT,
                    .chunk_sectors = 0,
                    .dev_sectors = UINT64_MAX,
                    .virt_boundary_mask = 0,
                },

            .discard =
                {
                    .discard_alignment = 0,
                    .discard_granularity = 0,
                    .max_discard_sectors = UINT_MAX >> SECTOR_SHIFT,
                    .max_write_zeroes_sectors = 0,
                    .max_discard_segments = 1,
                    .reserved0 = 0,
                },
            .devt = {0, 0, 0, 0},
            .zoned = {0, 0, 0, {0}},
            .dma =
                {
                    .alignment = 511,
                    .pad = {0},
                },
        })) {}

UblkDisk::~UblkDisk() = default;

io_result UblkDisk::handle_internal(ublksrv_queue const*, ublk_io_data const*, sub_cmd_t, iovec*, uint32_t, uint64_t,
                                    int) {
    return 0;
}

io_result UblkDisk::handle_rw(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, void* buf,
                              uint32_t const len, uint64_t const addr) {
    DLOGW("Use of deprecated ::handle_rw(...)! Please convert to using ::async_iov(...)")
    auto iov = iovec{.iov_base = buf, .iov_len = len};
    return async_iov(q, data, sub_cmd, &iov, 1, addr);
}

io_result UblkDisk::sync_io(uint8_t op, void* buf, size_t len, off_t addr) {
    DLOGW("Use of deprecated ::sync_io(...)! Please convert to using ::sync_iov(...)")
    auto iov = iovec{.iov_base = buf, .iov_len = len};
    return sync_iov(op, &iov, 1, addr);
}

io_result UblkDisk::queue_tgt_io(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd) {
    thread_local auto iov = iovec{.iov_base = nullptr, .iov_len = 0};

    DLOGT("Queue I/O [tag:{:#0x}] [sub_cmd:{}]", data->tag, ublkpp::to_string(sub_cmd))
    ublksrv_io_desc const* iod = data->iod;
    switch (ublksrv_get_op(iod)) {
    case UBLK_IO_OP_FLUSH:
        return handle_flush(q, data, sub_cmd);
    case UBLK_IO_OP_WRITE_ZEROES:
    case UBLK_IO_OP_DISCARD:
        return handle_discard(q, data, sub_cmd, iod->nr_sectors << SECTOR_SHIFT, iod->start_sector << SECTOR_SHIFT);
    case UBLK_IO_OP_READ:
    case UBLK_IO_OP_WRITE: {
        iov.iov_base = (void*)iod->addr;
        iov.iov_len = (iod->nr_sectors << SECTOR_SHIFT);
        return async_iov(q, data, sub_cmd, &iov, 1, (iod->start_sector << SECTOR_SHIFT));
    }
    default:
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }
}

io_result UblkDisk::queue_internal_resp(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, int res) {
    thread_local auto iov = iovec{.iov_base = nullptr, .iov_len = 0};
    ublksrv_io_desc const* iod = data->iod;
    iov.iov_base = (void*)iod->addr;
    iov.iov_len = (iod->nr_sectors << SECTOR_SHIFT);
    return handle_internal(q, data, sub_cmd, &iov, 1, iod->start_sector << SECTOR_SHIFT, res);
}

std::string UblkDisk::to_string() const {
    auto const cap_denom = capacity() >= Ti ? Gi : Mi;
    return fmt::format(
        "[{}, size={}{}, lbs={:#0x}, pbs={:#0x}, io_opt={:#0x}, io_min={:#0x}, max_tx={}Ki, discard={}{}]", id(),
        capacity() / cap_denom, cap_denom == Gi ? "Gi" : "Mi", block_size(), 1 << _params->basic.physical_bs_shift,
        1 << _params->basic.io_opt_shift, 1 << _params->basic.io_min_shift,
        (params()->basic.max_sectors << SECTOR_SHIFT) / Ki, can_discard(), direct_io ? "" : ", BUFFERED");
}
uint32_t UblkDisk::block_size() const { return 1 << _params->basic.logical_bs_shift; }
uint32_t UblkDisk::max_tx() const { return _params->basic.max_sectors << SECTOR_SHIFT; }
bool UblkDisk::can_discard() const { return _params->types & UBLK_PARAM_TYPE_DISCARD; }
uint64_t UblkDisk::capacity() const { return _params->basic.dev_sectors << SECTOR_SHIFT; }

} // namespace ublkpp
