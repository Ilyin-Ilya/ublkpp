#pragma once

extern "C" {
#include <sys/sysmacros.h>
}

#include <filesystem>
#include <fstream>
#include <string>

#include <sisl/logging/logging.h>
#include <ublksrv.h>

#include "lib/logging.hpp"

namespace ublkpp {

// In order to correctly handle partitions we follow the device link into the each subsystem rather than
// probe the sysfs/block filesystem which lacks discard info for partitions.
inline bool block_has_unmap(struct stat const& st) {
    static auto const sys_path = std::filesystem::path{"/"} / "sys" / "dev" / "block";
    static auto const discard_path = std::filesystem::path{"queue"} / "discard_max_hw_bytes";

    auto const subsysytem_link = sys_path / fmt::format("{}:{}", major(st.st_rdev), minor(st.st_rdev));
    auto ec = std::error_code();
    auto const resolved_path = std::filesystem::read_symlink(subsysytem_link, ec);
    if (ec) {
        DLOGW("Device [{}] is not present in sysfs [maj:min = {}:{}]: {}", subsysytem_link.native(), major(st.st_rdev),
              minor(st.st_rdev), ec.message())
        return false;
    }

    auto str_path = (sys_path / resolved_path / discard_path).native();
    DLOGD("Probing {}", str_path)
    std::ifstream discard_max(str_path, std::ios::in);
    if (!discard_max.is_open()) {
        str_path = (sys_path / resolved_path / ".." / discard_path).native();
        DLOGD("Testing for partition {}", str_path)
        discard_max = std::ifstream(str_path, std::ios::in);
    }
    if (!discard_max.is_open()) return false;

    uint64_t max_discard{0};
    std::string line;
    if (std::getline(discard_max, line)) {
        std::istringstream iss(line);
        iss >> max_discard;
    }
    return 0 < max_discard;
}

// High bit indicates this is a driver (e.g. FSDisk) I/O
inline uint64_t build_tgt_sqe_data(uint64_t tag, uint64_t op, uint64_t sub_cmd) {
    DEBUG_ASSERT_LE(tag, UINT16_MAX, "Tag too big: [{:#0x}]", tag)
    DEBUG_ASSERT_LE(op, UINT8_MAX, "Tag too big: [{:#0x}]", tag)
    DEBUG_ASSERT_LE(sub_cmd, UINT16_MAX, "Tag too big: [{:#0x}]", tag)
    return tag | (op << sqe_tag_width) | (sub_cmd << (sqe_tag_width + sqe_op_width)) |
        (static_cast< uint64_t >(0b1) << (sqe_tag_width + sqe_op_width + sqe_tgt_data_width + sqe_reserved_width));
}

inline auto discard_to_fallocate(ublksrv_io_desc const* iod) {
    int const mode = FALLOC_FL_KEEP_SIZE;
    if (UBLK_IO_OP_DISCARD == ublksrv_get_op(iod) || (0 == (UBLK_IO_F_NOUNMAP & ublksrv_get_flags(iod)))) {
        return mode | FALLOC_FL_PUNCH_HOLE;
    }
    return mode | FALLOC_FL_ZERO_RANGE;
}

} // namespace ublkpp
