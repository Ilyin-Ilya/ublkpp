#include "test_raid1_common.hpp"

// A v1 superblock (capacity-proportional reserved region) must open successfully.
// __init_params branches on sb_version to reconstruct the original on-disk layout.
TEST(Raid1, V1ArrayOpensWithLegacyLayout) {
    auto v1_sb_a = normal_superblock;
    v1_sb_a.header.version = htobe16(1);

    auto v1_sb_b = normal_superblock;
    v1_sb_b.header.version = htobe16(1);
    v1_sb_b.fields.device_b = 1;

    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([&v1_sb_a](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &v1_sb_a, ublkpp::raid1::k_page_size);
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([&v1_sb_b](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &v1_sb_b, ublkpp::raid1::k_page_size);
            return ublkpp::raid1::k_page_size;
        });

    // Neither device is new — no bitmap writes (init_to not called).
    // __become_active writes SB (clean_unmount=0) to both devices during construction.
    // Verify version stays 1 — __become_active must not bump it to 2.
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, 0))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            auto const* sb = static_cast< ublkpp::raid1::SuperBlock const* >(iovecs->iov_base);
            EXPECT_EQ(htobe16(1), sb->header.version);
            EXPECT_EQ(0, sb->fields.clean_unmount);
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, 0))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            auto const* sb = static_cast< ublkpp::raid1::SuperBlock const* >(iovecs->iov_base);
            EXPECT_EQ(htobe16(1), sb->header.version);
            EXPECT_EQ(0, sb->fields.clean_unmount);
            return ublkpp::raid1::k_page_size;
        });

    auto raid = ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // v1 reserved_size is capacity-proportional — smaller than the v2 fixed layout.
    EXPECT_LT(raid.reserved_size(),
              sizeof(ublkpp::raid1::SuperBlock) + ublkpp::raid1::k_superbitmap_bits * ublkpp::raid1::k_page_size);

    // Destructor writes SB (clean_unmount=1) to both devices.
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// A completely new array (both devices zero) must stamp SB_VERSION=2 on every write.
TEST(Raid1, NewArrayWritesV2Superblock) {
    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    auto device_b = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});

    auto zero_read = [](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
        memset(iovecs->iov_base, 0, iovecs->iov_len);
        return ublkpp::raid1::k_page_size;
    };
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _)).Times(1).WillOnce(zero_read);
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _)).Times(1).WillOnce(zero_read);

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, testing::Gt((off_t)0)))
        .Times(testing::AtLeast(1))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            return ublkpp::iovec_len(iovecs, iovecs + nr_vecs);
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, testing::Gt((off_t)0)))
        .Times(testing::AtLeast(1))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            return ublkpp::iovec_len(iovecs, iovecs + nr_vecs);
        });

    // Both SB writes at addr=0 during construction must carry version=2.
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, 0))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            auto const* sb = static_cast< ublkpp::raid1::SuperBlock const* >(iovecs->iov_base);
            EXPECT_EQ(htobe16(2), sb->header.version);
            return ublkpp::raid1::k_page_size;
        });
    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, 0))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            auto const* sb = static_cast< ublkpp::raid1::SuperBlock const* >(iovecs->iov_base);
            EXPECT_EQ(htobe16(2), sb->header.version);
            return ublkpp::raid1::k_page_size;
        });

    // assume_clean skips the new-array initial sync; this test asserts version stamping only.
    auto raid_device =
        ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b, "", true);
    // Destructor writes clean_unmount=1 SB to both devices.
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// A superblock with version > k_sb_version must be rejected — mirrors RAID0's M3 guard.
// device_b is StrictMock: the constructor must throw before ever reaching it.
TEST(Raid1, FutureSuperblockVersionThrows) {
    auto future_sb = normal_superblock;
    future_sb.header.version = htobe16(ublkpp::raid1::k_sb_version + 1);

    auto device_a = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi});
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(1)
        .WillOnce([&future_sb](uint8_t, iovec* iovecs, uint32_t, off_t) -> io_result {
            memcpy(iovecs->iov_base, &future_sb, ublkpp::raid1::k_page_size);
            return ublkpp::raid1::k_page_size;
        });
    auto device_b = std::make_shared< ::testing::StrictMock< ublkpp::TestDisk > >(TestParams{.capacity = Gi});

    EXPECT_THROW(ublkpp::raid1::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b),
                 std::runtime_error);
}
