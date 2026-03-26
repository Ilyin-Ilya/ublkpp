#include "test_raid0_common.hpp"

// Brief: Test that a simple DISCARD operation is received on all underlying devices
TEST(Raid0, MergedDiscard) {
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
    EXPECT_CALL(*device_b, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, uint32_t const len,
                     uint64_t const addr) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000001);
            EXPECT_EQ(len, 36 * Ki);
            EXPECT_EQ(addr, (32 * Ki) + (4 * Ki));
            return 1;
        });
    EXPECT_CALL(*device_c, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, uint32_t const len,
                     uint64_t const addr) {
            // Route is for Device C
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000010);
            EXPECT_EQ(len, 32 * Ki);
            EXPECT_EQ(addr, 32 * Ki);
            return 1;
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_DISCARD);
    auto const current_route = 0b10; // Pretend we've already gone through some upper layer

    // Set the address to within the second stripe (32KiB stripes)
    auto res = raid_device.handle_discard(nullptr, &ublk_data, current_route, 100 * Ki, 36 * Ki);
    ASSERT_TRUE(res);
    // Should have 3 total discards even though it wraps around to DeviceB twice
    EXPECT_EQ(3, res.value());
    remove_io_data(ublk_data);
}
