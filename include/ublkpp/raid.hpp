#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <sisl/utility/enum.hpp>

namespace ublkpp {

class ublk_disk;
using disk_handle = std::shared_ptr< ublk_disk >;

// Construct a RAID0 stripe set. `disks` becomes the array (ownership consumed).
// Throws std::runtime_error on bad geometry / superblock probe failure.
disk_handle make_raid0_disk(boost::uuids::uuid const& uuid, uint32_t stripe_size_bytes,
                            std::vector< disk_handle >&& disks);

// Construct a 2-way RAID1 mirror from `dev_a` + `dev_b`. `parent_id` is woven into the metrics
// labels; pass empty if metrics correlation is not needed.
// `assume_clean`: assert only that a genuinely fresh leg (no superblock) reads back zero for
// never-written blocks (e.g. a newly-provisioned thin volume): its rebuild then zero-detects the
// source and skips all-zero regions (ZERO_TEST) instead of blind-copying, and a brand-new pair
// (neither leg has a superblock) skips the md-style initial BLIND sync entirely (both legs already
// read identically). It never affects the compare-vs-blind choice -- a leg that still holds data
// always rebuilds in CHECK. Construction only; swap_device() takes its own. Leave false unless the
// device guarantees read-zero-when-unallocated. Like mdadm's --assume-clean, this is an unchecked
// operator assertion: if the device does not actually read zero where unallocated, ZERO_TEST skips
// non-zero destination extents and the legs diverge silently and permanently (undetectable until the
// surviving leg is needed).
// Throws std::runtime_error on bad geometry / superblock probe failure.
disk_handle make_raid1_disk(boost::uuids::uuid const& uuid, disk_handle dev_a, disk_handle dev_b,
                            std::string const& parent_id = "", bool assume_clean = false);

// Construct a placeholder disk representing a missing mirror leg. All I/O fails; is_missing()
// returns true. Pass to make_raid1_disk() when a leg is unavailable and awaiting hot-swap.
disk_handle make_missing_disk();

namespace raid0 {

// Returns the stripe leg at `stripe_offset`, or nullptr if out of range / not a Raid0 disk.
disk_handle get_device(ublk_disk const& disk, uint32_t stripe_offset) noexcept;

} // namespace raid0

namespace raid1 {

ENUM(replica_state, uint8_t, CLEAN = 0, SYNCING = 1, ERROR = 2, UNAVAIL = 3);

// Default-constructed value is a reserved sentinel: the implementation never produces both
// legs in ERROR simultaneously (the active leg is always CLEAN or UNAVAIL), so wrong-type
// queries are distinguishable from any valid Raid1 state.
struct array_state {
    replica_state device_a{replica_state::ERROR};
    replica_state device_b{replica_state::ERROR};
    uint64_t bytes_to_sync{0};
};

// Replace one leg of the mirror identified by `old_device_id` with `new_device`. On success
// returns the displaced leg. On rejection (no matching id, geometry mismatch, would-degrade-
// active-leg, etc.) returns `new_device` unchanged so the caller can identify rejection by
// pointer-equality. Aborts if `disk` is not a Raid1 mirror (programmer error).
// `assume_clean`: as on make_raid1_disk() -- assert `new_device` reads zero where unallocated so a
// genuinely-fresh rebuild runs ZERO_TEST instead of a blind copy. Same unchecked-operator-assertion
// caveat: a device that does not actually read zero where unallocated diverges the legs silently.
disk_handle swap_device(ublk_disk& disk, std::string const& old_device_id, disk_handle new_device,
                        bool assume_clean = false);

// Returns the per-leg replica state + bytes-to-sync. If `disk` is not a Raid1 mirror, returns
// the default-constructed value (see array_state).
array_state replica_states(ublk_disk const& disk) noexcept;

// Returns both legs of the mirror, or {nullptr, nullptr} if `disk` is not a Raid1 mirror.
std::pair< disk_handle, disk_handle > replicas(ublk_disk const& disk) noexcept;

} // namespace raid1

} // namespace ublkpp
