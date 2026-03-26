#include "test_raid0_common.hpp"

// Brief: Test that a simple FLUSH operation is received on all underlying devices
TEST(Raid0, SimpleFlush) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, handle_flush(_, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000000);
            return 1;
        });
    EXPECT_CALL(*device_b, handle_flush(_, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000001);
            return 1;
        });
    EXPECT_CALL(*device_c, handle_flush(_, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000010);
            return 1;
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_FLUSH);
    auto const current_route = 0b10; // Pretend we've already gone through some upper layer

    auto res = raid_device.handle_flush(nullptr, &ublk_data, current_route);
    ASSERT_TRUE(res);
    EXPECT_EQ(3, res.value());
    remove_io_data(ublk_data);
}
