#include "test_raid0_common.hpp"

// Brief: Test that a simple DISCARD RETRY operation is received on retried sub_cmd
TEST(Raid0, MergedDiscardRetry) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, uint32_t const len,
                     uint64_t const addr) {
            // Route is for Device A
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000000);
            EXPECT_EQ(len, 32 * Ki);
            EXPECT_EQ(addr, (64 * Ki));
            return 1;
        });
    EXPECT_CALL(*device_b, handle_discard(_, _, _, _, _)).Times(0);
    EXPECT_CALL(*device_c, handle_discard(_, _, _, _, _)).Times(0);

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_DISCARD);
    auto const retried_route = ublkpp::set_flags(ublkpp::sub_cmd_t{0b10000000}, ublkpp::sub_cmd_flags::RETRIED);

    // Set the address to within the second stripe (32KiB stripes)
    auto res = raid_device.handle_discard(nullptr, &ublk_data, retried_route, 100 * Ki, 36 * Ki);
    ASSERT_TRUE(res);
    // Should have 1 total discards retries
    EXPECT_EQ(1, res.value());
    remove_io_data(ublk_data);
}
