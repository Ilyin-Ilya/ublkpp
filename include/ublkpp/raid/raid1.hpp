#pragma once

#include <memory>

#include <boost/uuid/uuid.hpp>
#include <ublkpp/lib/ublk_disk.hpp>

namespace ublkpp {

class UblkRaidMetrics;

namespace raid1 {
class Raid1DiskImpl;
ENUM(replica_state, uint8_t, CLEAN = 0, SYNCING = 1, ERROR = 2);
struct array_state {
    replica_state device_a;
    replica_state device_b;
    uint64_t bytes_to_sync;
};
} // namespace raid1
class Raid1Disk : public UblkDisk {
public:
    Raid1Disk(boost::uuids::uuid const& uuid, std::shared_ptr< UblkDisk > dev_a, std::shared_ptr< UblkDisk > dev_b,
              std::string const& parent_id = "");
    ~Raid1Disk() override;

    /// Raid1Disk API
    /// =============
    std::shared_ptr< UblkDisk > swap_device(std::string const& old_device_id, std::shared_ptr< UblkDisk > new_device);
    raid1::array_state replica_states() const;
    uint64_t reserved_size() const;
    std::pair< std::shared_ptr< UblkDisk >, std::shared_ptr< UblkDisk > > replicas() const;
    void toggle_resync(bool t);
    /// =============

    /// UBlkDisk Interface Overrides
    /// ============================
    uint32_t block_size() const override;
    bool can_discard() const override;
    uint64_t capacity() const override;
    // ================

    ublk_params* params() override;
    ublk_params const* params() const override;
    std::string id() const override;
    std::list< int > open_for_uring(int const iouring_device) override;

    uint8_t route_size() const override;

    void idle_transition(ublksrv_queue const*, bool) override;

    void on_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) override;

    io_result handle_internal(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovec,
                              uint32_t nr_vecs, uint64_t addr, int res) override;
    void collect_async(ublksrv_queue const*, std::list< async_result >& compl_list) override;
    // RAID-1 Devices can not sit on-top of non-O_DIRECT devices, so there's nothing to flush
    io_result handle_flush(ublksrv_queue const*, ublk_io_data const*, sub_cmd_t) override;
    io_result handle_discard(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                             uint64_t addr) override;

    io_result async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                        uint32_t nr_vecs, uint64_t addr) override;
    io_result sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept override;
    /// ============================

private:
    std::unique_ptr< raid1::Raid1DiskImpl > _impl;
};
} // namespace ublkpp
