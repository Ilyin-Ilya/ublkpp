#include "../test_raid1_common.hpp"

// Test: open_for_uring collects FDs from both devices
TEST(Raid1, OpenForUring) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    // Set up expectations for open_for_uring
    EXPECT_CALL(*device_a, open_for_uring(0)).Times(1).WillOnce(::testing::Return(std::list< int >{10, 11}));

    EXPECT_CALL(*device_b, open_for_uring(2)).Times(1).WillOnce(::testing::Return(std::list< int >{12}));

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto fds = raid_device.open_for_uring(0);

    // Should have 3 FDs total (2 from device_a, 1 from device_b)
    ASSERT_EQ(fds.size(), 3);

    auto it = fds.begin();
    EXPECT_EQ(*it++, 10);
    EXPECT_EQ(*it++, 11);
    EXPECT_EQ(*it++, 12);

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test: open_for_uring with no FDs from devices
TEST(Raid1, OpenForUringEmpty) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    // Both devices return empty lists
    EXPECT_CALL(*device_a, open_for_uring(0)).Times(1).WillOnce(::testing::Return(std::list< int >{}));

    EXPECT_CALL(*device_b, open_for_uring(0)).Times(1).WillOnce(::testing::Return(std::list< int >{}));

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto fds = raid_device.open_for_uring(0);

    EXPECT_TRUE(fds.empty());

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test: open_for_uring offset calculation
TEST(Raid1, OpenForUringOffsetCalculation) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    // Device A returns 3 FDs, so device B should get offset of 103 (100 + 3)
    EXPECT_CALL(*device_a, open_for_uring(100)).Times(1).WillOnce(::testing::Return(std::list< int >{5, 6, 7}));

    EXPECT_CALL(*device_b, open_for_uring(103)).Times(1).WillOnce(::testing::Return(std::list< int >{8, 9}));

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto fds = raid_device.open_for_uring(100);

    ASSERT_EQ(fds.size(), 5);

    // Expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
