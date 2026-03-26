#include "../test_raid1_common.hpp"

// Test: idle_transition enter (manages resync state, doesn't propagate to devices)
TEST(Raid1, IdleTransitionEnter) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Should not crash - manages internal resync state
    raid_device.idle_transition(nullptr, true);

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test: idle_transition exit (manages resync state, doesn't propagate to devices)
TEST(Raid1, IdleTransitionExit) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Should not crash - manages internal resync state
    raid_device.idle_transition(nullptr, false);

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test: collect_async with no pending results
TEST(Raid1, CollectAsyncEmpty) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    std::list< ublkpp::async_result > results;
    raid_device.collect_async(nullptr, results);

    EXPECT_TRUE(results.empty());

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test: handle_flush always returns 0 (RAID1 requires direct I/O)
TEST(Raid1, HandleFlush) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto ublk_data = make_io_data(UBLK_IO_OP_FLUSH);
    auto res = raid_device.handle_flush(nullptr, &ublk_data, 0);
    remove_io_data(ublk_data);

    // RAID1 always returns 0 for flush (no buffered I/O)
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res.value(), 0);

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
