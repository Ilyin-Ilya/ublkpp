#include "test_raid1_common.hpp"

TEST(Raid1, SyncIoWriteFailA) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    auto const test_op = UBLK_IO_OP_WRITE;
    auto const test_off = 8 * Ki;
    auto const test_sz = 12 * Ki;

    EXPECT_SYNC_OP(test_op, device_a, false, true, test_sz, test_off + raid_device.reserved_size()); // Fail this write
    EXPECT_CALL(*device_b, sync_iov(test_op, _, _, _))
        .Times(3)
        .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            EXPECT_EQ(ublkpp::raid1::k_page_size, iov->iov_len);
            EXPECT_EQ(0UL, addr);
            return iov->iov_len;
        })
        .WillOnce([](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            EXPECT_GE(addr, ublkpp::raid1::k_page_size);  // Expect write to bitmap!
            EXPECT_LT(addr, raid_device.reserved_size()); // Expect write to bitmap!
            return iov->iov_len;
        })
        .WillOnce([test_sz](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            EXPECT_EQ(test_sz, iov->iov_len);
            EXPECT_EQ(test_off + raid_device.reserved_size(), addr);
            return iov->iov_len;
        });

    auto res = raid_device.sync_io(test_op, nullptr, test_sz, test_off);
    ASSERT_TRUE(res);
    // No need to re-write on A side
    EXPECT_EQ(test_sz, res.value());

    // expect unmount_clean on Device B
    EXPECT_TO_WRITE_SB(device_b);
}
