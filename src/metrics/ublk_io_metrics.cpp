#include "ublk_io_metrics.hpp"

#include <ublksrv.h>

namespace ublkpp {

UblkIOMetrics::UblkIOMetrics(std::string const& uuid)
    : sisl::MetricsGroup{"ublk_io_metrics", uuid} {
    REGISTER_HISTOGRAM(ublk_read_queue_distribution, "Read queue depth distribution", HistogramBucketsType(ExponentialOfTwoBuckets));
    REGISTER_HISTOGRAM(ublk_write_queue_distribution, "Write queue depth distribution", HistogramBucketsType(ExponentialOfTwoBuckets));
    register_me_to_farm();
}

UblkIOMetrics::~UblkIOMetrics() { deregister_me_from_farm(); }

void UblkIOMetrics::record_queue_depth_change(ublksrv_queue const* q, uint8_t op, bool is_increment) {
    if (!q || !q->private_data) return;

    // UBLK_IO_OP_READ = 0, UBLK_IO_OP_WRITE = 1
    if (op == 0) { // UBLK_IO_OP_READ
        if (is_increment) {
            auto const depth = _queued_reads.fetch_add(1, std::memory_order_relaxed) + 1;
            HISTOGRAM_OBSERVE(*this, ublk_read_queue_distribution, depth);
        } else {
            _queued_reads.fetch_sub(1, std::memory_order_relaxed);
        }
    } else if (op == 1) { // UBLK_IO_OP_WRITE
        if (is_increment) {
            auto const depth = _queued_writes.fetch_add(1, std::memory_order_relaxed) + 1;
            HISTOGRAM_OBSERVE(*this, ublk_write_queue_distribution, depth);
        } else {
            _queued_writes.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

} // namespace ublkpp
