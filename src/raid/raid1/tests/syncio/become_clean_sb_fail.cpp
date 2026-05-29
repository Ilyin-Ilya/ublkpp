#include "test_raid1_common.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace ublkpp::raid1;

// Regression test for RAID1-B1: __become_clean must not CAS to EITHER when the active device
// SB write fails.  If the CAS fires despite the failure, in-memory route becomes EITHER while
// on-disk route stays DEVA — diverged state on next restart (startup would see clean SB on
// device_a but route=DEVA on device_b and pick the wrong device as primary).
//
// Setup: degraded array (route=DEVA, both devices present, clean bitmap).  toggle_resync(true)
// causes the resync thread to fire complete() immediately (0 dirty pages), which calls
// __become_clean.  The active device write is made to fail.  With the fix, __become_clean
// returns without the CAS so the shutdown SB still shows route=DEVA; without the fix it would
// show EITHER.
TEST(Raid1, BecomeCleaActiveWriteFailNoCas) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .is_slot_b = true});

    // device_a: SB with route=DEVA, age=2 (wins pick_superblock)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t, off_t addr) -> io_result {
            if (addr == 0UL) {
                memcpy(iovecs->iov_base, &normal_superblock, k_page_size);
                auto* sb = static_cast< SuperBlock* >(iovecs->iov_base);
                sb->fields.read_route = static_cast< uint8_t >(read_route::DEVA);
                sb->fields.device_b = 0;
                sb->fields.clean_unmount = 1;
                sb->fields.bitmap.age = htobe64(2);
            } else {
                memset(iovecs->iov_base, 0x00, iovecs->iov_len); // empty bitmap pages
            }
            return k_page_size;
        });

    // device_b: SB with route=DEVA, age=1 (loses pick_superblock; age diff == 1 → not new_device)
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &normal_superblock, k_page_size);
            auto* sb = static_cast< SuperBlock* >(iovecs->iov_base);
            sb->fields.read_route = static_cast< uint8_t >(read_route::DEVA);
            sb->fields.device_b = 1;
            sb->fields.clean_unmount = 1;
            sb->fields.bitmap.age = htobe64(1);
            return k_page_size;
        });

    // device_b accepts any writes (only gets the __become_active SB write; no destructor write
    // since is_degraded=true; no __become_clean write since active write fails first with fix)
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .WillRepeatedly([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result { return iov->iov_len; });

    std::mutex mtx;
    std::condition_variable cv;
    bool become_clean_attempted = false;
    read_route shutdown_route = read_route::EITHER; // initialised to EITHER to detect absence

    // Writes to device_a at offset 0:
    //   1st: __become_active succeeds
    //   2nd: __become_clean active write — fail it and signal the test
    //   later: destructor shutdown SB — capture the route
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, (off_t)0))
        .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result { return iov->iov_len; })
        .WillOnce([&](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            {
                std::lock_guard< std::mutex > lk(mtx);
                become_clean_attempted = true;
            }
            cv.notify_one();
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        })
        .WillRepeatedly([&](uint8_t, iovec* iov, uint32_t, off_t) -> io_result {
            auto const* sb = static_cast< SuperBlock const* >(iov->iov_base);
            if (sb->fields.clean_unmount) shutdown_route = static_cast< read_route >(sb->fields.read_route);
            return iov->iov_len;
        });

    // Bitmap-area writes are accepted but not expected to occur (empty bitmap)
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, testing::Gt((off_t)0)))
        .WillRepeatedly([](uint8_t, iovec* iov, uint32_t, off_t) -> io_result { return iov->iov_len; });

    {
        auto raid = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
        raid.toggle_resync(true);

        std::unique_lock< std::mutex > lk(mtx);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(2), [&] { return become_clean_attempted; }))
            << "resync complete callback did not fire within 2 seconds";
    } // destructor: stop() joins resync thread, then writes shutdown SB to device_a with current route

    EXPECT_EQ(read_route::DEVA, shutdown_route)
        << "RAID1-B1: CAS to EITHER must not fire when active SB write fails in __become_clean";
}
