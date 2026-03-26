#include "test_raid0_common.hpp"

// Brief: Test a READ through the RAID0 Device. We should only receive the READ on one of the
// three underlying stripes.
TEST(Raid0, SimpleRead) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                     uint32_t nr_vecs, uint64_t addr) -> io_result {
            // The route should shift up by 4
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000000);
            EXPECT_FALSE(ublkpp::is_retry(sub_cmd));
            EXPECT_EQ(nr_vecs, 1);
            EXPECT_EQ(iovecs->iov_len, 4 * Ki);
            EXPECT_EQ(addr - (32 * Ki), 8 * Ki);
            return 1;
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);
    EXPECT_CALL(*device_c, async_iov(_, _, _, _, _, _)).Times(0);

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_READ);
    auto const current_route = 0b10; // Pretend we've already gone through some upper layer
    auto res = raid_device.handle_rw(nullptr, &ublk_data, current_route, nullptr, 4 * Ki, 8 * Ki);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());
    remove_io_data(ublk_data);
}
