#pragma once

#include <cstdint>

#include <fmt/format.h>
#include <sisl/utility/enum.hpp>

namespace ublkpp {

using sub_cmd_t = uint16_t;
constexpr auto sqe_tag_width = 16U;
constexpr auto sqe_op_width = 8U;
constexpr auto sqe_tgt_data_width = sizeof(sub_cmd_t) * 8U;
constexpr auto sqe_is_tgt_width = 1U;
constexpr auto sqe_reserved_width = 64U - (sqe_tag_width + sqe_op_width + sqe_tgt_data_width + sqe_is_tgt_width);

// Device Specific Flags
constexpr auto _flag_width = 8U;
constexpr auto _route_width = sqe_tgt_data_width - _flag_width;

/// SubCmd flags that Devices and the Target set to communicate State of the SubCmd encoded into io_uring user_data
//
// * NONE       - Normal I/O request from the client.
// * REPLICATE  - Replicated I/O (e.g. RAID1) that duplicates above.
// * RETRIED    - SubCmd that _failed_ and has been reissued by the Target.
// * DEPENDENT  - Does not contribute to User request, but *must* succeed.
// * INTERNAL   - Does not contribute to the success/failure of any User request.
ENUM(sub_cmd_flags, sub_cmd_t, NONE = 0, REPLICATE = 1, RETRIED = 2, DEPENDENT = 4, INTERNAL = 8)

inline sub_cmd_t set_flags(sub_cmd_t sub_cmd, sub_cmd_flags const flags) {
    return sub_cmd | (static_cast< sub_cmd_t >(flags) << _route_width);
}

inline sub_cmd_t unset_flags(sub_cmd_t sub_cmd, sub_cmd_flags const flags) {
    return sub_cmd & ~(static_cast< sub_cmd_t >(flags) << _route_width);
}

inline bool test_flags(sub_cmd_t sub_cmd, sub_cmd_flags const flags) {
    return 0 < ((sub_cmd >> _route_width) & static_cast< sub_cmd_t >(flags));
}

inline auto is_replicate(sub_cmd_t sub_cmd) { return test_flags(sub_cmd, sub_cmd_flags::REPLICATE); }
inline auto is_retry(sub_cmd_t sub_cmd) { return test_flags(sub_cmd, sub_cmd_flags::RETRIED); }
inline auto is_dependent(sub_cmd_t sub_cmd) { return test_flags(sub_cmd, sub_cmd_flags::DEPENDENT); }
inline auto is_internal(sub_cmd_t sub_cmd) { return test_flags(sub_cmd, sub_cmd_flags::INTERNAL); }

// SubCmd routing for Error Handling
constexpr auto _route_mask = (1U << _route_width) - 1U;

inline sub_cmd_t shift_route(sub_cmd_t sub_cmd, sub_cmd_t const shift) { return (_route_mask & sub_cmd) << shift; }

inline auto to_string(sub_cmd_t const& sub_cmd) {
    return fmt::format("{{{:#02x}:{:08b}}}", sub_cmd >> _route_width, (sub_cmd & _route_mask));
}
} // namespace ublkpp
