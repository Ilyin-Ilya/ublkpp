#include "test_raid1_common.hpp"

// Brief: Test that a simple WRITE operation is replicated to both underlying Devices.
TEST(Raid1, SimpleWrite) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            // Route is for Device A
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
            EXPECT_FALSE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 16 * Ki);
            EXPECT_EQ(addr, (12 * Ki) + raid_device.reserved_size());
            return 1;
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
            // SubCommand has the replicated bit set
            EXPECT_TRUE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 16 * Ki);
            EXPECT_EQ(addr, (12 * Ki) + raid_device.reserved_size());
            return 1;
        });

    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE, 16 * Ki, 12 * Ki);
    // Construct a Retry Route that points to Device A in a RAID1 device
    auto const current_sub_cmd = 0b10;
    auto res = raid_device.queue_tgt_io(nullptr, &ublk_data, current_sub_cmd);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(2, res.value());

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
