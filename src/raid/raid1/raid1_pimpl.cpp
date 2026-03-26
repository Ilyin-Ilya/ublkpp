#include "raid1_impl.hpp"
#include "metrics/ublk_raid_metrics.hpp"

namespace ublkpp {

/// Raid1Disk Public Class
Raid1Disk::Raid1Disk(boost::uuids::uuid const& uuid, std::shared_ptr< UblkDisk > dev_a,
                     std::shared_ptr< UblkDisk > dev_b, std::string const& parent_id) :
        _impl(std::make_unique< raid1::Raid1DiskImpl >(uuid, dev_a, dev_b, parent_id)) {
    direct_io = _impl->direct_io;
    uses_ublk_iouring = _impl->uses_ublk_iouring;
}

Raid1Disk::~Raid1Disk() = default;

std::shared_ptr< UblkDisk > Raid1Disk::swap_device(std::string const& old_device_id,
                                                   std::shared_ptr< UblkDisk > new_device) {
    return _impl->swap_device(old_device_id, new_device);
}
raid1::array_state Raid1Disk::replica_states() const { return _impl->replica_states(); }
uint64_t Raid1Disk::reserved_size() const { return _impl->get_reserved_size(); }
std::pair< std::shared_ptr< UblkDisk >, std::shared_ptr< UblkDisk > > Raid1Disk::replicas() const {
    return _impl->replicas();
}
void Raid1Disk::toggle_resync(bool t) { return _impl->toggle_resync(t); }

uint32_t Raid1Disk::block_size() const { return _impl->block_size(); }
bool Raid1Disk::can_discard() const { return _impl->can_discard(); }
uint64_t Raid1Disk::capacity() const { return _impl->capacity(); }

ublk_params* Raid1Disk::params() { return _impl->params(); }
ublk_params const* Raid1Disk::params() const { return _impl->params(); }
std::string Raid1Disk::id() const { return _impl->id(); }
std::list< int > Raid1Disk::open_for_uring(int const iouring_device) { return _impl->open_for_uring(iouring_device); }
uint8_t Raid1Disk::route_size() const { return _impl->route_size(); }
void Raid1Disk::idle_transition(ublksrv_queue const* q, bool enter) { return _impl->idle_transition(q, enter); }
void Raid1Disk::on_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) { return _impl->on_io_complete(data, sub_cmd); }

io_result Raid1Disk::handle_internal(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovec,
                                     uint32_t nr_vecs, uint64_t addr, int res) {
    return _impl->handle_internal(q, data, sub_cmd, iovec, nr_vecs, addr, res);
}
void Raid1Disk::collect_async(ublksrv_queue const* q, std::list< async_result >& compl_list) {
    return _impl->collect_async(q, compl_list);
}
io_result Raid1Disk::handle_flush(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd) {
    return _impl->handle_flush(q, data, sub_cmd);
}
io_result Raid1Disk::handle_discard(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                                    uint64_t addr) {
    return _impl->handle_discard(q, data, sub_cmd, len, addr);
}
io_result Raid1Disk::async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                               uint32_t nr_vecs, uint64_t addr) {
    return _impl->async_iov(q, data, sub_cmd, iovecs, nr_vecs, addr);
}
io_result Raid1Disk::sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept {
    return _impl->sync_iov(op, iovecs, nr_vecs, offset);
}
// ================
} // namespace ublkpp
