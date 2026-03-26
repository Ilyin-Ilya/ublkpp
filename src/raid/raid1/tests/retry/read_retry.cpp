#include "test_raid1_common.hpp"

// Brief: Test retrying a READ through the RAID1 Device, and subsequent READs now go to B
//
// A failed read does not prevent us from continuing to try and read from the device, it must
// experience a failure to mutate, so this immediate read failure still has the follow-up read
// attempt on device A.
TEST(Raid1, ReadRetryA) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b11);
            // It should also have the RETRIED bit set
            EXPECT_TRUE(ublkpp::is_retry(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr, (12 * Ki) + raid_device.reserved_size());
            return 1;
        });

    auto ublk_data = make_io_data(UBLK_IO_OP_READ, 4 * Ki, 12 * Ki);
    // Construct a Retry Route that points to Device A in a RAID1 device
    auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b10}, ublkpp::sub_cmd_flags::RETRIED);
    auto res = raid_device.queue_tgt_io(nullptr, &ublk_data, sub_cmd);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());

    // Now test the normal path
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            // The route has changed to point to device_b
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
            // It should not have the RETRIED bit set
            EXPECT_FALSE(ublkpp::is_retry(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr, (12 * Ki) + raid_device.reserved_size());
            return 1;
        });

    ublk_data = make_io_data(UBLK_IO_OP_READ);
    // Construct a Non-Retry Route
    sub_cmd = ublkpp::sub_cmd_t{0b10};
    res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 4 * Ki, 12 * Ki);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Brief: Identical to ReadRetryA but for Device B.
TEST(Raid1, ReadRetryB) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
            // It should also have the RETRIED bit set
            EXPECT_TRUE(ublkpp::is_retry(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 64 * Ki);
            EXPECT_EQ(addr, (32 * Ki) + raid_device.reserved_size());
            return 1;
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);

    auto ublk_data = make_io_data(UBLK_IO_OP_READ);
    auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101}, ublkpp::sub_cmd_flags::RETRIED);
    auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 64 * Ki, 32 * Ki);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
