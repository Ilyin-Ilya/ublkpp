#include "test_raid1_common.hpp"

// Fail a write that crosses pages on the bitmap dirtying two pages
TEST(Raid1, WriteFailAcrossPages) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = 2 * Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = 2 * Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    {
        auto const test_op = UBLK_IO_OP_WRITE;
        auto const test_off = (1 * Gi) - (4 * Ki);
        auto const test_sz = 40 * Ki;

        EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz,
                       test_off + raid_device.reserved_size()); // Fail this write
        EXPECT_CALL(*device_b, sync_iov(test_op, _, _, _))
            .Times(2)
            .WillOnce([&raid_device](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
                EXPECT_EQ(ublkpp::raid1::k_page_size, iov->iov_len);
                EXPECT_EQ(0UL, addr);
                return iov->iov_len;
            })
            .WillOnce([test_sz, &raid_device](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
                EXPECT_EQ(test_sz, iov->iov_len);
                EXPECT_EQ(test_off + raid_device.reserved_size(), addr);
                return iov->iov_len;
            });

        auto res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
        ASSERT_TRUE(res);
        // No need to re-write on A side
        EXPECT_EQ(test_sz, res.value());
    }

    // expect unmount_clean on Device B
    EXPECT_TO_WRITE_SB(device_b);

    // Flush dirty bitmap - with batching, two consecutive pages are written in one call
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([&raid_device](uint8_t, iovec*, uint32_t nr_vecs, off_t addr) -> io_result {
            EXPECT_EQ(2U, nr_vecs);                                // 2 consecutive pages batched
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);           // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size());          // Expect write to bitmap!
            return nr_vecs * ublkpp::raid1::k_page_size;          // Return total bytes written
        })
        .RetiresOnSaturation();
}
