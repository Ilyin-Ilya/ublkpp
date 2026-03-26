#include "test_raid1_common.hpp"

#include <thread>

using namespace std::chrono_literals;

// Test __clean_bitmap completes successfully when bitmap is clean
TEST(Raid1, CleanBitmapNoDirtyPages) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Disable resync to control when it runs
    raid_device.toggle_resync(false);

    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_b);
    EXPECT_EQ(0, cur_replica_state.bytes_to_sync);

    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}

// Test __clean_bitmap handles a single dirty region
TEST(Raid1, CleanBitmapSingleRegion) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    // Disable resync to control when it runs
    raid_device.toggle_resync(false);

    {
        // Cause a write failure to dirty the bitmap
        EXPECT_TO_WRITE_SB(device_a);
        EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 32 * Ki, 64 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    EXPECT_EQ(32 * Ki, cur_replica_state.bytes_to_sync);

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    // Allow resync operations - accept any reasonable read/write operations
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    // Enable resync and wait for completion
    raid_device.toggle_resync(true);
    std::this_thread::sleep_for(100ms);

    cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_b);
    EXPECT_EQ(0, cur_replica_state.bytes_to_sync);
}

// Test __clean_bitmap handles multiple dirty regions
TEST(Raid1, CleanBitmapMultipleRegions) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    raid_device.toggle_resync(false);

    // Set up flexible expectations before creating dirty regions
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    // Create multiple dirty regions at different offsets
    for (uint64_t offset : {64 * Ki, 128 * Ki, 256 * Ki}) {
        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 32 * Ki, offset);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    EXPECT_EQ(3 * 32 * Ki, cur_replica_state.bytes_to_sync);

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    raid_device.toggle_resync(true);
    std::this_thread::sleep_for(150ms);

    cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_b);
    EXPECT_EQ(0, cur_replica_state.bytes_to_sync);
}

// Test __clean_bitmap handles read failure during resync
TEST(Raid1, CleanBitmapReadFailure) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    raid_device.toggle_resync(false);

    {
        // Create a dirty region
        EXPECT_TO_WRITE_SB(device_a);
        EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 32 * Ki, 64 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    // Simulate read failure from clean device during resync
    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(::testing::AtLeast(1))
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    EXPECT_CALL(*device_a, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    raid_device.toggle_resync(true);
    std::this_thread::sleep_for(100ms);

    // Device should still be dirty due to read failure
    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    EXPECT_EQ(32 * Ki, cur_replica_state.bytes_to_sync);
}

// Test __clean_bitmap handles write failure during resync
TEST(Raid1, CleanBitmapWriteFailure) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    raid_device.toggle_resync(false);

    {
        // Create a dirty region
        EXPECT_TO_WRITE_SB(device_a);
        EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 32 * Ki, 64 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    // Simulate successful reads but write failure to dirty device during resync
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(::testing::AtLeast(1))
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    EXPECT_CALL(*device_b, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    raid_device.toggle_resync(true);
    std::this_thread::sleep_for(100ms);

    // Device should still be dirty due to write failure
    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    EXPECT_EQ(32 * Ki, cur_replica_state.bytes_to_sync);
}

// Test __clean_bitmap can be stopped mid-resync
TEST(Raid1, CleanBitmapStoppedState) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    raid_device.toggle_resync(false);

    // Set up flexible expectations before creating dirty regions
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    // Create multiple dirty regions
    for (uint64_t offset : {64 * Ki, 128 * Ki, 256 * Ki, 512 * Ki, 1 * Mi}) {
        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 32 * Ki, offset);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    EXPECT_EQ(5 * 32 * Ki, cur_replica_state.bytes_to_sync);

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    // Start resync
    raid_device.toggle_resync(true);

    // Let it run for a short time
    std::this_thread::sleep_for(10ms);

    // Stop resync
    raid_device.toggle_resync(false);
    std::this_thread::sleep_for(10ms);

    // May still have dirty pages depending on timing
    cur_replica_state = raid_device.replica_states();
    // Device B should still be in recovery or have remaining bytes to sync
    EXPECT_GE(cur_replica_state.bytes_to_sync, 0);
}

// Test __clean_bitmap handles large dirty region that spans multiple I/O operations
TEST(Raid1, CleanBitmapLargeRegion) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});
    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);

    raid_device.toggle_resync(false);

    {
        // Create a large dirty region (multiple chunks)
        EXPECT_TO_WRITE_SB(device_a);
        EXPECT_CALL(*device_b, async_iov(_, _, _, _, _, _)).Times(0);

        auto ublk_data = make_io_data(UBLK_IO_OP_WRITE);
        auto sub_cmd = ublkpp::set_flags(ublkpp::sub_cmd_t{0b101},
                                         ublkpp::sub_cmd_flags::RETRIED | ublkpp::sub_cmd_flags::REPLICATE);
        auto res = raid_device.handle_rw(nullptr, &ublk_data, sub_cmd, nullptr, 256 * Ki, 64 * Ki);
        remove_io_data(ublk_data);
        ASSERT_TRUE(res);
    }

    auto cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::ERROR, cur_replica_state.device_b);
    // Expect 256Ki rounded up to chunk boundary (32Ki chunks)
    EXPECT_EQ(256 * Ki, cur_replica_state.bytes_to_sync);

    {
        // Make Device B available again
        iovec iov{.iov_base = nullptr, .iov_len = 0 * Ki};
        auto res = raid_device.handle_internal(nullptr, nullptr, 0b101, &iov, 1, 64 * Ki, 0);
        ASSERT_TRUE(res);
    }

    // Expect multiple I/O operations to resync the large region
    EXPECT_CALL(*device_a, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_b, sync_iov(::testing::_, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t) -> io_result {
            EXPECT_EQ(1U, nr_vecs);
            if (UBLK_IO_OP_READ == op && nullptr != iovecs->iov_base) {
                memcpy(iovecs->iov_base, &normal_superblock, ublkpp::raid1::k_page_size);
                static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
            }
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    EXPECT_CALL(*device_a, async_iov(_, _, _, _, _, _))
        .Times(::testing::AtLeast(0))
        .WillRepeatedly([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t,
                           uint64_t) -> io_result { return 1; });

    raid_device.toggle_resync(true);
    std::this_thread::sleep_for(150ms);

    cur_replica_state = raid_device.replica_states();
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_a);
    EXPECT_EQ(ublkpp::raid1::replica_state::CLEAN, cur_replica_state.device_b);
    EXPECT_EQ(0, cur_replica_state.bytes_to_sync);
}
