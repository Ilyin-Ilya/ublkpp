#include "ublk_fsdisk_metrics.hpp"

#include <iostream>
#include <ublksrv.h>

namespace ublkpp {

UblkFSDiskMetrics::UblkFSDiskMetrics(std::string const& parent_id, std::string const& disk_path)
    : sisl::MetricsGroup{disk_path, disk_path} {
    // Use disk_path as entity name to ensure each disk has unique metrics
    // Use raid_uuid as label so you can filter by RAID in Sherlock/Prometheus

    REGISTER_HISTOGRAM(disk_io_latency_us, "Disk I/O latency in microseconds", "ublk_disk_io_latency_us",
                       {"parent_id", parent_id}, HistogramBucketsType(ExponentialOfTwoBuckets));

    register_me_to_farm();
}

UblkFSDiskMetrics::~UblkFSDiskMetrics() { deregister_me_from_farm(); }

void UblkFSDiskMetrics::record_io_start(ublk_io_data const* data, sub_cmd_t sub_cmd) {
    if (!data) return;

    auto key = std::make_pair(static_cast<uint16_t>(data->tag), static_cast<uint16_t>(sub_cmd));
    t_disk_io_timings[key] = io_timing{std::chrono::steady_clock::now()};
}

void UblkFSDiskMetrics::record_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) {
    if (!data) return;

    auto key = std::make_pair(static_cast<uint16_t>(data->tag), static_cast<uint16_t>(sub_cmd));

    if (auto it = t_disk_io_timings.find(key); it != t_disk_io_timings.end()) {
        auto const& timing = it->second;
        auto const end_time = std::chrono::steady_clock::now();
        auto const latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - timing.start_time).count();

        HISTOGRAM_OBSERVE(*this, disk_io_latency_us, latency_us);
        t_disk_io_timings.erase(it);
    }
}

} // namespace ublkpp
