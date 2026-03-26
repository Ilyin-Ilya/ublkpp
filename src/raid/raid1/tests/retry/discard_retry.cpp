#include "test_raid1_common.hpp"

//  Failed Discards should flip the route and dirty the bitmap too
TEST(Raid1, DiscardRetry) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
    raid_device.toggle_resync(false);

    {
        EXPECT_TO_WRITE_SB(device_a);
        EXPECT_CALL(*device_a, handle_discard(_, _, _, _, _)).Times(0);
        EXPECT_CALL(*device_b, handle_discard(_, _, _, _, _)).Times(0);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_discard(nullptr, &ublk_data, sub_cmd, 4 * Ki, 8 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
        // No need to re-write on A side
        EXPECT_EQ(0, res.value());
    }
    auto compls = std::list< ublkpp::async_result >();
    raid_device.collect_async(nullptr, compls);
    EXPECT_EQ(compls.size(), 0);
    compls.clear();

    // Subsequent reads should not go to device B
    auto ublk_data = make_io_data(UBLK_IO_OP_READ);
    EXPECT_CALL(*device_a, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                 uint32_t, uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
            EXPECT_FALSE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr, (8 * Ki) + raid_device.reserved_size());
            return 1;
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);
    auto res = raid_device.handle_rw(nullptr, &ublk_data, 0b10, nullptr, 4 * Ki, 8 * Ki);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());
    // expect unmount_clean on Device A
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .WillOnce([&raid_device](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);  // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size()); // Expect write to bitmap!
            return iov->iov_len;
        })
        .RetiresOnSaturation();
}
