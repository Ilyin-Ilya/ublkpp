#include "../test_raid0_common.hpp"

// Test: async_iov with invalid nr_vecs (0)
TEST(Raid0, AsyncIovInvalidNrVecs) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    auto ublk_data = make_io_data(UBLK_IO_OP_READ);

    // Pass 0 for nr_vecs - should return error
    auto res = raid_device.async_iov(nullptr, &ublk_data, 0, nullptr, 0, 0);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::make_error_condition(std::errc::invalid_argument));

    remove_io_data(ublk_data);
}

// Test: handle_internal with invalid nr_vecs (0)
TEST(Raid0, HandleInternalInvalidNrVecs) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    auto ublk_data = make_io_data(UBLK_IO_OP_READ);

    // Pass 0 for nr_vecs - should return error
    auto res = raid_device.handle_internal(nullptr, &ublk_data, 0, nullptr, 0, 0, 0);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::make_error_condition(std::errc::invalid_argument));

    remove_io_data(ublk_data);
}

// Test: async_iov error propagation from device
TEST(Raid0, AsyncIovErrorPropagation) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    // Device A will return an error
    EXPECT_CALL(*device_a, async_iov(UBLK_IO_OP_READ, _, _, _, _, _))
        .Times(1)
        .WillOnce(
            [](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, iovec*, uint32_t, uint64_t) -> io_result {
                return std::unexpected(std::make_error_condition(std::errc::io_error));
            });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    auto ublk_data = make_io_data(UBLK_IO_OP_READ);

    // Access first device - should propagate error
    auto res = raid_device.handle_rw(nullptr, &ublk_data, 0, nullptr, 4 * Ki, 0);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::make_error_condition(std::errc::io_error));

    remove_io_data(ublk_data);
}

// Test: handle_flush error propagation
TEST(Raid0, HandleFlushErrorPropagation) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    // Device A returns success
    EXPECT_CALL(*device_a, handle_flush(_, _, _)).Times(1).WillOnce(Return(io_result{1}));

    // Device B returns error - should stop and propagate
    EXPECT_CALL(*device_b, handle_flush(_, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    auto ublk_data = make_io_data(UBLK_IO_OP_FLUSH);
    auto res = raid_device.handle_flush(nullptr, &ublk_data, 0);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::make_error_condition(std::errc::io_error));

    remove_io_data(ublk_data);
}

// Test: handle_discard error propagation
TEST(Raid0, HandleDiscardErrorPropagation) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    // First device returns error
    EXPECT_CALL(*device_a, handle_discard(_, _, _, _, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, ublk_io_data const*, ublkpp::sub_cmd_t, uint32_t, uint64_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    auto ublk_data = make_io_data(UBLK_IO_OP_DISCARD);
    auto res = raid_device.handle_discard(nullptr, &ublk_data, 0, 4 * Ki, 0);

    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::make_error_condition(std::errc::io_error));

    remove_io_data(ublk_data);
}

// Test: get_device with invalid offset
TEST(Raid0, GetDeviceInvalidOffset) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});

    // Try to get device beyond array size
    auto device = raid_device.get_device(5);

    EXPECT_EQ(device, nullptr);
}

// Test: get_device with valid offsets
TEST(Raid0, GetDeviceValidOffsets) {
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_c = CREATE_DISK(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b, device_c});

    // All valid offsets should return non-null
    EXPECT_NE(raid_device.get_device(0), nullptr);
    EXPECT_NE(raid_device.get_device(1), nullptr);
    EXPECT_NE(raid_device.get_device(2), nullptr);

    // Verify they're the right devices
    EXPECT_EQ(raid_device.get_device(0), device_a);
    EXPECT_EQ(raid_device.get_device(1), device_b);
    EXPECT_EQ(raid_device.get_device(2), device_c);
}
