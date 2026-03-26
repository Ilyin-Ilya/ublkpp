#include "test_raid0_common.hpp"

// Brief: Test that a simple WRITE operation is again only received on a single stripe.
TEST(Raid0, SimpleWrite) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                     uint32_t nr_vecs, uint64_t addr) {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000000);
            EXPECT_EQ(nr_vecs, 1);
            EXPECT_EQ(iovecs->iov_len, 16 * Ki);
            // This is the first chunk of the second device
            EXPECT_EQ(addr, (36 * Ki) + (32 * Ki));
            return 1;
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a});

    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
    auto const current_route = 0b10; // Pretend we've already gone through some upper layer

    // Set the address to within the second stripe (32KiB stripes)
    auto res = raid_device.handle_rw(nullptr, &ublk_data, current_route, nullptr, 16 * Ki, 36 * Ki);
    ASSERT_TRUE(res);
    EXPECT_EQ(1, res.value());
    remove_io_data(ublk_data);
}
