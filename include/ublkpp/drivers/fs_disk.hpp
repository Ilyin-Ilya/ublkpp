#pragma once

#include <filesystem>
#include <memory>

#include <ublkpp/lib/ublk_disk.hpp>

namespace ublkpp {

class UblkFSDiskMetrics;

class FSDisk : public UblkDisk {
    std::filesystem::path _path;
    int _fd{-1};
    bool _block_device{false};
    std::unique_ptr<UblkFSDiskMetrics> _metrics;

public:
    // Constructor: creates metrics internally using parent_id
    explicit FSDisk(std::filesystem::path const& path, std::string const& parent_id = "");
    ~FSDisk() override;

    std::string id() const override { return _path.native(); }

    io_result handle_flush(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd) override;
    io_result handle_discard(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, uint32_t len,
                             uint64_t addr) override;

    io_result async_iov(ublksrv_queue const* q, ublk_io_data const* data, sub_cmd_t sub_cmd, iovec* iovecs,
                        uint32_t nr_vecs, uint64_t addr) override;
    io_result sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t offset) noexcept override;

    void on_io_complete(ublk_io_data const* data, sub_cmd_t sub_cmd) override;
};
} // namespace ublkpp
