#include "test_raid1_common.hpp"

// Immediate Write Fail
TEST(Raid1, WriteFailImmediateFailFailSBUpdate) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    {
        EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
            .Times(1)
            .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                         uint64_t const) { return std::unexpected(std::make_error_condition(std::errc::io_error)); });
        EXPECT_TO_WRITE_SB_F(device_b, true); // Fail the attempt to dirty the SB

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, 0b10, nullptr, 4 * Ki, 8 * Ki);
        remove_io_data(ublk_data);
        ASSERT_FALSE(res);
    }

    // Subsequent writes should continue to try device A this time succeeding dirty of header and bitmap update
    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
    EXPECT_TO_WRITE_SB(device_b);
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t, uint64_t const) {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(2)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs, uint32_t,
                     uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);  // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size()); // Expect write to bitmap!
            return 1;
        })
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs, uint32_t,
                     uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
            EXPECT_FALSE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr, (8 * Ki) + raid_device.reserved_size());
            return 1;
        });
    auto res = raid_device.handle_rw(nullptr, &ublk_data, 0b10, nullptr, 4 * Ki, 8 * Ki);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(2, res.value());

    // expect unmount_clean on Device B
    EXPECT_TO_WRITE_SB(device_b);
}
