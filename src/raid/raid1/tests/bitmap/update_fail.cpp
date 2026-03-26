#include "test_raid1_common.hpp"

// Test the failure to update the BITMAP but not the SB header itself
TEST(Raid1, BITMAPUpdateFail) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
    raid_device.toggle_resync(false);

    {
        auto const test_op = UBLK_IO_OP_WRITE;
        auto const test_off = 8 * Ki;
        auto const test_sz = 12 * Ki;

        EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz, test_off + raid_device.reserved_size());
        EXPECT_CALL(*device_b, sync_iov(test_op, _, _, _))
            .Times(2)
            .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
                EXPECT_EQ(ublkpp::raid1::k_page_size, iov->iov_len);
                EXPECT_EQ(0UL, addr);
                return iov->iov_len;
            })
            .WillOnce([test_sz, test_off, &raid_device](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
                EXPECT_EQ(test_sz, iov->iov_len);
                EXPECT_EQ(test_off + raid_device.reserved_size(), addr);
                return iov->iov_len;
            });

        auto res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
        EXPECT_TRUE(res);
    }

    // Subsequent reads should not go to device A
    auto ublk_data = make_io_data(UBLK_IO_OP_READ);
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(*device_b, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
            EXPECT_FALSE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr, (8 * Ki) + raid_device.reserved_size());
            return 1;
        });
    auto res = raid_device.handle_rw(nullptr, &ublk_data, 0b10, nullptr, 4 * Ki, 8 * Ki);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .WillOnce([&raid_device](uint8_t, iovec*, uint32_t, off_t addr) -> io_result {
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);  // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size()); // Expect write to bitmap!
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });
}
