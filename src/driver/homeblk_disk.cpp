#include "ublkpp/drivers/homeblk_disk.hpp"

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <homeblks/volume_mgr.hpp>
#include <sisl/logging/logging.h>
#include <ublksrv.h>

#include "lib/logging.hpp"

namespace ublkpp {

constexpr auto s_physical_block_size = 4 * Ki;

HomeBlkDisk::HomeBlkDisk(boost::uuids::uuid const& homeblk_vol_id, uint64_t capacity,
                         std::shared_ptr< homeblocks::VolumeManager > hb_vol_if, uint32_t const _max_tx) :
        _vol_id(homeblk_vol_id), _hb_vol_if(hb_vol_if), _hb_volume(_hb_vol_if->lookup_volume(_vol_id)) {
    if (!_hb_volume) throw std::runtime_error("Failed to lookup volume!");

    direct_io = true;
    uses_ublk_iouring = false;

    // Setup parameters for ublk device
    auto& our_params = *params();
    auto const lbs = 4 * Ki;
    auto const pbs = s_physical_block_size;
    auto const sz = capacity;
    DLOGD("Device Parameters [vol_id={}] [sz:{},lbs:{},pbs:{}]", sz, boost::uuids::to_string(_vol_id), lbs, pbs)
    our_params.basic.logical_bs_shift = static_cast< uint8_t >(ilog2(lbs));
    our_params.basic.physical_bs_shift = static_cast< uint8_t >(ilog2(pbs));
    our_params.basic.dev_sectors = sz >> SECTOR_SHIFT;
    our_params.basic.max_sectors = _max_tx >> SECTOR_SHIFT;

    // TODO set this when enabling DISCARD
    if (UINT32_MAX == our_params.discard.discard_granularity) {
        our_params.discard.discard_granularity = 0;
        our_params.types &= ~UBLK_PARAM_TYPE_DISCARD;
    }
}

HomeBlkDisk::~HomeBlkDisk() = default;

io_result HomeBlkDisk::handle_flush(ublksrv_queue const*, ublk_io_data const*, sub_cmd_t) {
    DEBUG_ASSERT(direct_io, "DirectIO not enabled and received FLUSH!");
    return 0;
}

struct ublk_vol_req : homeblocks::vol_interface_req {
    static boost::intrusive_ptr< ublk_vol_req > make(void* write_buffer, uint64_t lba_in, uint32_t nblks,
                                                     homeblocks::VolumePtr& vol_ptr) {
        return boost::intrusive_ptr< ublk_vol_req >(new ublk_vol_req((uint8_t*)write_buffer, lba_in, nblks, vol_ptr));
    }
    void free_yourself() override { delete this; }

    ublk_vol_req(uint8_t* write_buffer, uint64_t lba_in, uint32_t nblks, homeblocks::VolumePtr vol_ptr) :
            homeblocks::vol_interface_req(write_buffer, lba_in, nblks, vol_ptr) {}
};

void HomeBlkDisk::collect_async(ublksrv_queue const*, std::list< async_result >& completed) {
    auto lck = std::scoped_lock< std::mutex >(pending_results_lck);
    completed.splice(completed.end(), std::move(pending_results));
}

io_result HomeBlkDisk::handle_discard(ublksrv_queue const*, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                                      uint64_t addr) {
    // TODO Implement discard
    DLOGD("DISCARD [vol_id:{}]: [tag:{:#0x}] ublk io [sector:{}|len:{}|sub_cmd:{}]", boost::uuids::to_string(_vol_id),
          data->tag, addr >> SECTOR_SHIFT, len, ublkpp::to_string(sub_cmd))
    return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
}

io_result HomeBlkDisk::async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t nr_vecs, uint64_t addr) {
    // HomeBlks does not support vectorized I/O
    if (1 != nr_vecs) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));

    auto const op = ublksrv_get_op(data->iod);
    auto const nr_lbas = __iovec_len(iovecs, iovecs + nr_vecs) >> params()->basic.logical_bs_shift;
    addr = addr >> params()->basic.logical_bs_shift;
    DLOGT("{} [vol_id:{}] : [tag:{:#0x}] ublk io [lba:{}|nr_lbas:{}|sub_cmd:{}]",
          op == UBLK_IO_OP_READ ? "READ" : "WRITE", boost::uuids::to_string(_vol_id), data->tag, addr, nr_lbas,
          ublkpp::to_string(sub_cmd))

    auto new_request = ublk_vol_req::make(iovecs->iov_base, addr, nr_lbas, _hb_volume);

    ((UBLK_IO_OP_READ == op) ? _hb_vol_if->read(_hb_volume, new_request) : _hb_vol_if->write(_hb_volume, new_request))
        .thenValue([this, new_request, q, data, sub_cmd, nr_lbas, addr](auto&& e) mutable {
            DLOGT("I/O complete [vol_id:{}] : [tag:{:#0x}] ublk io [lba:{}|nr_lbas:{}|sub_cmd:{}]",
                  boost::uuids::to_string(_vol_id), data->tag, addr, nr_lbas, ublkpp::to_string(sub_cmd));
            {
                auto lck = std::scoped_lock< std::mutex >(pending_results_lck);
                pending_results.emplace_back(async_result{
                    data, sub_cmd, (!e.has_value() ? -EIO : (int)nr_lbas << params()->basic.logical_bs_shift)});
            }
            ublksrv_queue_send_event(q);
            new_request.reset();
        });
    return 1;
}

io_result HomeBlkDisk::sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) noexcept {
    // HomeBlks does not support vectorized I/O
    if (1 != nr_vecs) return std::unexpected(std::make_error_condition(std::errc::invalid_argument));

    DLOGT("{} [vol_id:{}] : [INTERNAL] ublk io [sector:{}|len:{}]", op == UBLK_IO_OP_READ ? "READ" : "WRITE",
          boost::uuids::to_string(_vol_id), addr >> SECTOR_SHIFT, __iovec_len(iovecs, iovecs + nr_vecs))
    // TODO
    return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
}

} // namespace ublkpp
