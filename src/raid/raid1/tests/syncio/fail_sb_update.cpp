#include "test_raid1_common.hpp"

// This test fails the initial sync_io to the working device and then fails the SB update to dirty the bitmap on
// the replica. The I/O should fail in this case.
TEST(Raid1, SyncIoWriteFailDirty) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto const test_op = UBLK_IO_OP_WRITE;
    auto const test_off = 8 * Ki;
    auto const test_sz = 12 * Ki;

    EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz, test_off + raid_device.reserved_size());
    EXPECT_SB_OP(test_op, device_b, true, true);

    ASSERT_FALSE(raid_device.sync_io(UBLK_IO_OP_WRITE, nullptr, test_sz, test_off));

    // Even though I/O failed, the status is still OK since devices are in same state pre-I/O
    // expect attempt to sync on last working disk
    EXPECT_TO_WRITE_SB_F(device_a, true);
    EXPECT_TO_WRITE_SB_F(device_b, true);
}
