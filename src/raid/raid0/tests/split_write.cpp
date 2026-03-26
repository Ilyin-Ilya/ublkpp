#include "test_raid0_common.hpp"

// Test that a overlapping I/O is correct Split and then formed into multiple iovec structures
TEST(Raid0, SplitWrite) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});
    void* fake_buffer;
    ASSERT_EQ(0, posix_memalign(&fake_buffer, device_a->block_size(), 96 * Ki));
    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([fake_buffer](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                uint32_t nr_vecs, uint64_t addr) -> io_result {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000000);
            EXPECT_EQ(nr_vecs, 1);
            EXPECT_EQ(iovecs->iov_len, 32 * Ki);
            EXPECT_EQ((uint8_t*)iovecs->iov_base, (uint8_t*)fake_buffer + (60 * Ki));
            EXPECT_EQ(addr, (32 * Ki) * 2);
            return 1;
        });
    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([fake_buffer](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                uint32_t nr_vecs, uint64_t addr) -> io_result {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000001);
            EXPECT_EQ(nr_vecs, 2);
            EXPECT_EQ(ublkpp::__iovec_len(iovecs, iovecs + nr_vecs), 32 * Ki);
            EXPECT_EQ((uint8_t*)iovecs->iov_base, (uint8_t*)fake_buffer);
            EXPECT_EQ((uint8_t*)(iovecs + 1)->iov_base, (uint8_t*)fake_buffer + (92 * Ki));
            // This is the first chunk of the second device
            EXPECT_EQ(addr - (32 * Ki), (36 * Ki) - (32 * Ki));
            return 1;
        });
    EXPECT_CALL(*device_c, async_iov(_, _, _, _, _, _))
        .Times(1)
        .WillOnce([fake_buffer](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t sub_cmd, iovec* iovecs,
                                uint32_t nr_vecs, uint64_t addr) -> io_result {
            // Route is for Device B
            EXPECT_EQ(sub_cmd & ublkpp::_route_mask, 0b10000010);
            EXPECT_EQ(nr_vecs, 1);
            EXPECT_EQ(iovecs->iov_len, 32 * Ki);
            EXPECT_EQ((uint8_t*)iovecs->iov_base, (uint8_t*)fake_buffer + (28 * Ki));
            // This is the first chunk of the second device
            EXPECT_EQ(addr, (32 * Ki));
            return 1;
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);

    // Set the address to within the second stripe (32KiB stripes)
    auto res = raid_device.handle_rw(nullptr, &ublk_data, 0b10, fake_buffer, 96 * Ki, 36 * Ki);
    ASSERT_TRUE(res);
    EXPECT_EQ(3, res.value());
    remove_io_data(ublk_data);
    free(fake_buffer);
}
