#include "test_raid0_common.hpp"

TEST(Raid0, RetrySplitWritePortion) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                     uint32_t nr_vecs, uint64_t addr) -> io_result {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000001);
            EXPECT_EQ(nr_vecs, 1);
            EXPECT_EQ(iovecs->iov_len, 28 * Ki);
            // This is the first chunk of the second device
            EXPECT_EQ(addr - (32 * Ki), (36 * Ki) - (32 * Ki));
            return 1;
        });
    EXPECT_CALL(*device_c, async_iov(_, _, _, _, _, _)).Times(0);

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
    auto const retried_route = ublkpp::set_flags(ublkpp::sub_cmd_t{0b10000001}, ublkpp::sub_cmd_flags::RETRIED);

    // Set the address to within the second stripe (32KiB stripes)
    auto res = raid_device.handle_rw(nullptr, &ublk_data, retried_route, nullptr, 44 * Ki, 36 * Ki);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());
    remove_io_data(ublk_data);
}
