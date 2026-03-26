#include "ublk_raid_metrics.hpp"

#include <fmt/format.h>

namespace ublkpp {

UblkRaidMetrics::UblkRaidMetrics(std::string const& parent_id, std::string const& raid_device_id)
    : sisl::MetricsGroup{raid_device_id, raid_device_id} {
    // Use raid_device_id as entity name to ensure each RAID has unique metrics
    // Use parent_id (app parent_id) as label so you can filter by application in Sherlock/Prometheus

    REGISTER_COUNTER(raid_degraded_count_device_a, "RAID device A degradation events", "ublk_raid_degraded_count_device_a",
                     {"parent_id", parent_id});
    REGISTER_COUNTER(raid_degraded_count_device_b, "RAID device B degradation events", "ublk_raid_degraded_count_device_b",
                     {"parent_id", parent_id});
    REGISTER_COUNTER(device_swaps_total, "Total number of device swaps", "ublk_device_swaps_total",
                     {"parent_id", parent_id});
    // RAID1 resync metrics
    REGISTER_COUNTER(resync_started_total, "Total number of resyncs started", "ublk_resync_started_total",
                     {"parent_id", parent_id});
    REGISTER_HISTOGRAM(resync_progress_kib, "Resync chunk sizes in KiB", "ublk_resync_progress_kib",
                       {"parent_id", parent_id}, HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_HISTOGRAM(resync_duration_s, "Resync duration in seconds", "ublk_resync_duration_s",
                       {"parent_id", parent_id}, HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_GAUGE(active_resyncs, "Number of active resyncs", "ublk_active_resyncs",
                   {"parent_id", parent_id});
    REGISTER_GAUGE(dirty_pages, "Number of dirty bitmap pages", "ublk_dirty_pages",
                   {"parent_id", parent_id});
    REGISTER_GAUGE(last_resync_size_kib, "Size of last resync in KiB", "ublk_last_resync_size_kib",
                   {"parent_id", parent_id});
    register_me_to_farm();
}

UblkRaidMetrics::~UblkRaidMetrics() { deregister_me_from_farm(); }

void UblkRaidMetrics::record_device_degraded(std::string const& device_name) {
    if (device_name == "device_a") {
        COUNTER_INCREMENT(*this, raid_degraded_count_device_a, 1);
    } else if (device_name == "device_b") {
        COUNTER_INCREMENT(*this, raid_degraded_count_device_b, 1);
    }
}

void UblkRaidMetrics::record_resync_start() {
    COUNTER_INCREMENT(*this, resync_started_total, 1);
}

void UblkRaidMetrics::record_resync_progress(uint64_t bytes) {
    // Track resync chunk sizes as histogram in KiB
    // Convert to KiB (1 KiB = 1024 bytes)
    uint64_t kibibytes = bytes / 1024;
    HISTOGRAM_OBSERVE(*this, resync_progress_kib, kibibytes);
}

void UblkRaidMetrics::record_resync_complete(uint64_t duration_seconds) {
    HISTOGRAM_OBSERVE(*this, resync_duration_s, duration_seconds);
}

void UblkRaidMetrics::record_active_resyncs(uint64_t count) {
    GAUGE_UPDATE(*this, active_resyncs, count);
}

void UblkRaidMetrics::record_dirty_pages(uint64_t pages) {
    GAUGE_UPDATE(*this, dirty_pages, pages);
}

void UblkRaidMetrics::record_device_swap() {
    COUNTER_INCREMENT(*this, device_swaps_total, 1);
}

void UblkRaidMetrics::record_last_resync_size(uint64_t bytes) {
    // Track last resync size in KiB
    // Convert to KiB (1 KiB = 1024 bytes)
    uint64_t kibibytes = bytes / 1024;
    GAUGE_UPDATE(*this, last_resync_size_kib, kibibytes);
}

} // namespace ublkpp
