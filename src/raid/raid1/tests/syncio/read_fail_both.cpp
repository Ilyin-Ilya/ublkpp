#include "test_raid1_common.hpp"

// This test is similar to SyncIoDevAFail, but the re-issued READ fails too. Still device is *NOT* degraded!
TEST(Raid1, SyncIoReadFailBoth) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto const test_op = UBLK_IO_OP_READ;
    auto const test_off = 64 * Ki;
    auto const test_sz = 1024 * Ki;

    EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz, test_off + raid_device.reserved_size());
    EXPECT_SYNC_OP(test_op, device_b, true, true, test_sz, test_off + raid_device.reserved_size());

    EXPECT_FALSE(raid_device.sync_io(test_op, nullptr, test_sz, test_off));

    // expect attempt to sync both SBs
    EXPECT_TO_WRITE_SB_F(device_a, true);
    EXPECT_TO_WRITE_SB_F(device_b, true);
}
