#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include <boost/uuid/uuid.hpp>

#include "metrics/ublk_io_metrics.hpp"

struct ublksrv_ctrl_dev;
struct ublksrv_dev;
struct ublksrv_dev_data;
struct ublksrv_tgt_type;

namespace ublkpp {

class UblkDisk;

struct ublkpp_tgt_impl {
    bool device_added{false};
    bool device_recovering{false};
    boost::uuids::uuid volume_uuid;
    std::filesystem::path device_path;
    // Owned by us
    std::shared_ptr< UblkDisk > device;
    std::unique_ptr< ublksrv_tgt_type const > tgt_type;

    // Owned by libublksrv
    ublksrv_ctrl_dev* ctrl_dev{nullptr};
    ublksrv_dev const* ublk_dev{nullptr};

    // == Metrics ==
    UblkIOMetrics metrics;

    std::atomic_uint32_t _queued_reads;
    std::atomic_uint32_t _queued_writes;
    // == ======= ==

    // Owned by us
    std::unique_ptr< ublksrv_dev_data > dev_data;
    std::vector< std::thread > queue_handlers;

    ublkpp_tgt_impl(boost::uuids::uuid const& vol_id, std::shared_ptr< UblkDisk > d);
    ~ublkpp_tgt_impl();
    void destroy();
};

} // namespace ublkpp
