#include "test_raid1_common.hpp"

// Test basic __failover_read functionality
TEST(Raid1, SyncIoReadDevAFail) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto const test_op = UBLK_IO_OP_READ;
    auto const test_off = 8 * Ki;
    auto const test_sz = 80 * Ki;

    EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz, test_off + raid_device.reserved_size());
    EXPECT_SYNC_OP(test_op, device_b, true, false, test_sz, test_off + raid_device.reserved_size());

    auto res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
    ASSERT_TRUE(res);
    EXPECT_EQ(test_sz, res.value());

    // expect unmount_clean on both (READ fails do not dirty bitmap)
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
