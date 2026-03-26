#include "test_raid1_common.hpp"

// Brief: Degrade the array and then fail read; it should not attempt failover read from dirty regions
TEST(Raid1, ReadOnDegraded) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
    raid_device.toggle_resync(false);

    // First send a retry write to degrade the array on side A
    {
        // Will dirty superblock header
        EXPECT_TO_WRITE_SB(device_b);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b100}, ublkpp::sub_cmd_flags::RETRIED);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 4 * Ki, 8 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
        EXPECT_EQ(1, res.value());
    }

    // Now send retry reads for the region; they should fail immediately
    {
        auto ublk_data = make_io_data(UBLK_IO_OP_READ);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101}, ublkpp::sub_cmd_flags::RETRIED);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 64 * Ki, 4 * Ki);
        remove_io_data(ublk_data);
        EXPECT_FALSE(res);
    }
    // Retries from non-dirty chunks go through
    {
        EXPECT_CALL(*device_a, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
            .Times(1)
            .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd,
                                     iovec* iovecs, uint32_t, uint64_t addr) {
                EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
                // It should also have the RETRIED bit set
                EXPECT_TRUE(ublkpp::is_retry(sub_cmd));
                EXPECT_EQ(iovecs->iov_len, 64 * Ki);
                EXPECT_EQ(addr, (128 * Ki) + raid_device.reserved_size());
                return 1;
            });
        auto ublk_data = make_io_data(UBLK_IO_OP_READ);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101}, ublkpp::sub_cmd_flags::RETRIED);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 64 * Ki, 128 * Ki);
        remove_io_data(ublk_data);
        EXPECT_TRUE(res);
    }
    // Retries from the degraded device are fine
    {
        EXPECT_CALL(*device_b, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
            .Times(1)
            .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd,
                                     iovec* iovecs, uint32_t, uint64_t addr) {
                EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
                // It should also have the RETRIED bit set
                EXPECT_TRUE(ublkpp::is_retry(sub_cmd));
                EXPECT_EQ(iovecs->iov_len, 64 * Ki);
                EXPECT_EQ(addr, (4 * Ki) + raid_device.reserved_size());
                return 1;
            });
        auto ublk_data = make_io_data(UBLK_IO_OP_READ);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b100}, ublkpp::sub_cmd_flags::RETRIED);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 64 * Ki, 4 * Ki);
        remove_io_data(ublk_data);
        EXPECT_TRUE(res);
    }

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_b);

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .WillOnce([&raid_device](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);  // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size()); // Expect write to bitmap!
            return iov->iov_len;
        })
        .RetiresOnSaturation();
}
