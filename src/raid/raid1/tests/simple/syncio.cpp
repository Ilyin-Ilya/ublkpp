#include "test_raid1_common.hpp"

// Test basic R/W on the Raid1Disk::sync_io
TEST(Raid1, SimpleSyncIo) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto test_op = UBLK_IO_OP_READ;
    auto test_off = 8 * Ki;
    auto test_sz = 12 * Ki;

    // Reads will only go to device_a at start
    EXPECT_SYNC_OP(test_op, device_a, false, false, test_sz, test_off + raid_device.reserved_size());

    auto res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
    ASSERT_TRUE(res);
    EXPECT_EQ(test_sz, res.value());

    test_op = UBLK_IO_OP_WRITE;
    test_off = 1024 * Ki;
    test_sz = 16 * Ki;

    EXPECT_SYNC_OP(test_op, device_a, false, false, test_sz, test_off + raid_device.reserved_size());
    EXPECT_SYNC_OP(test_op, device_b, true, false, test_sz, test_off + raid_device.reserved_size());

    res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
    ASSERT_TRUE(res);
    EXPECT_EQ(test_sz, res.value());

    // expect unmount_clean on devices
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
