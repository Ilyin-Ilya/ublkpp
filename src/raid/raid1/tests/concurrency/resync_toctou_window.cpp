#include "test_raid1_common.hpp"

#include <atomic>
#include <boost/uuid/string_generator.hpp>
#include <thread>

#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/raid1_impl.hpp"
#include "raid/raid1/raid1_resync_task.hpp"

using namespace std::chrono_literals;
using namespace ublkpp::raid1;

// Regression test for RAID1-B6: TOCTOU between dirty_pages()==0 and complete().
//
// The race: resync loop exits because dirty_pages()==0. Before complete() returns, a concurrent
// write dirtied a page against the active device only (route was still DEVA/DEVB). complete()
// transitions route to EITHER so all subsequent writes go to both mirrors — but the page dirtied
// in the window is stranded: it reached the active mirror only and was never copied to the
// backup.
//
// The fix: after calling complete(), re-check dirty_pages(). If >0, run one extra __run() pass.
// Since route is now EITHER, no new dirty pages can accumulate; one pass cleans the window.
//
// Test strategy: use Raid1ResyncTask directly (as in resync_relaunch_after_complete.cpp).
// Start with an empty bitmap (0 dirty pages). The complete callback dirtifies chunk 0 before
// returning, simulating the TOCTOU window write. After stop() joins the resync thread, assert
// dirty_pages()==0 — the extra pass must have cleaned the window-dirty page.
TEST(Raid1Concurrency, ResyncToctouWindow) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "DiskA"});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "DiskB", .is_slot_b = true});

    // device_a is clean_mirror; device_b is dirty_mirror (being resynced)
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(sync_iov_zero_on_read());
    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AnyNumber())
        .WillRepeatedly(sync_iov_zero_on_read());

    auto uuid = boost::uuids::string_generator()(test_uuid);
    auto mirror_a = std::make_shared< MirrorDevice >(uuid, device_a); // clean mirror
    auto mirror_b = std::make_shared< MirrorDevice >(uuid, device_b); // dirty mirror

    constexpr uint32_t chunk_size = 32 * Ki;
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = std::make_shared< Bitmap >(Gi, chunk_size, 4 * Ki, superbitmap_buf.get());

    // max_size must equal chunk_size: clean_region() asserts chunk alignment on its len argument,
    // and __run() copies at most max_size bytes per iteration (which becomes the len passed to __clean).
    Raid1ResyncTask task{bitmap, Bitmap::page_size(), chunk_size, chunk_size};

    std::atomic< bool > complete_fired{false};

    // The complete callback simulates a write that arrives in the TOCTOU window: it dirties
    // chunk 0 before returning, so dirty_pages() is 1 when the re-check runs.
    task.launch(test_uuid, mirror_a, mirror_b, [&] {
        bitmap->dirty_region(0, chunk_size);
        complete_fired.store(true, std::memory_order_release);
    });

    // Wait for the complete callback with a 2-second timeout (sanity: resync should be fast)
    auto const deadline = std::chrono::steady_clock::now() + 2s;
    while (!complete_fired.load(std::memory_order_acquire)) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "resync complete callback did not fire";
        std::this_thread::sleep_for(1ms);
    }

    // stop() joins the resync thread — the thread must have finished its extra __run() pass by now
    task.stop();

    EXPECT_EQ(0u, bitmap->dirty_pages())
        << "RAID1-B6: page dirtied in TOCTOU window must be cleaned by the extra __run() pass after complete()";
}
