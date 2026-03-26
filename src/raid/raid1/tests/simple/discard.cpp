#include "test_raid1_common.hpp"

TEST(Raid1, SimpleDiscard) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    EXPECT_CALL(*device_a, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, uint32_t len,
                                 uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b100);
            EXPECT_FALSE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(len, 4 * Ki);
            EXPECT_EQ(addr, (8 * Ki) + raid_device.reserved_size());
            return 1;
        });
    EXPECT_CALL(*device_b, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, uint32_t len,
                                 uint64_t addr) {
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b101);
            EXPECT_TRUE(ublkpp::is_replicate(sub_cmd));
            EXPECT_EQ(len, 4 * Ki);
            EXPECT_EQ(addr, (8 * Ki) + raid_device.reserved_size());
            return 1;
        });

    auto ublk_data = make_io_data(UBLK_IO_OP_DISCARD, 4 * Ki, 8 * Ki);
    auto res = raid_device.queue_tgt_io(nullptr, &ublk_data, 0b10);
    remove_io_data(ublk_data);
    ASSERT_TRUE(res);
    EXPECT_EQ(2, res.value());
    // expect unmount_clean on devices
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
