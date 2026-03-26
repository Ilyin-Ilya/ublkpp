#pragma once

#include <boost/uuid/uuid.hpp>
#include <ublkpp/lib/ublk_disk.hpp>

namespace ublkpp {

struct StripeDevice;

constexpr uint32_t _max_stripe_cnt{64};

class Raid0Disk : public UblkDisk {
    std::vector< std::unique_ptr< StripeDevice > > _stripe_array;

    uint32_t _stripe_size{0};
    uint32_t _stride_width{0};


    io_result __distribute(iovec* iov, uint64_t addr, auto&& func, bool retry = false, sub_cmd_t sub_cmd = 0) const;

public:
    Raid0Disk(boost::uuids::uuid const& uuid, uint32_t const stripe_size_bytes,
              std::vector< std::shared_ptr< UblkDisk > >&& disks);
    ~Raid0Disk() override;

    /// Raid0Disk API
    /// =============
    std::shared_ptr< UblkDisk > get_device(uint32_t stripe_offset) const;
    uint32_t stripe_size() const { return _stripe_size; }
    /// =============

    /// UBlkDisk Interface Overrides
    /// ============================
    std::string id() const override { return "RAID0"; }
    std::list< int > open_for_uring(int const iouring_device) override;

    uint8_t route_size() const override { return ilog2(_max_stripe_cnt); }
    void idle_transition(ublksrv_queue const*, bool) override;

    void collect_async(ublksrv_queue const*, std::list< async_result >& compl_list) override;
    io_result handle_internal(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                              uint32_t nr_vecs, uint64_t addr, int res) override;
    io_result handle_flush(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd) override;
    io_result handle_discard(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                             uint64_t addr) override;

    io_result async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                        uint32_t nr_vecs, uint64_t addr) override;

    io_result sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept override;

    void on_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) override;
    /// ============================
};
} // namespace ublkpp
