#include "test_raid0_common.hpp"

// Brief: Test that we open the underlying devices correctly, and return them to our upper layer.
//
// When a UblkDisk receives a call to `open_for_uring`, it's expected to return a std::set of all
// fds that were opened by the underlying Devices in order to register them with io_uring. Test
// that RAID0 is collecting these FDs and passing the io_uring offset to the lower layers.
TEST(Raid0, OpenDevices) {
    static const auto start_idx = 2;
    auto device_a = CREATE_DISK(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK(TestParams{.capacity = Gi});

    // Each device should be subsequently opened and return a set with their sole FD.
    EXPECT_CALL(*device_a, open_for_uring(_)).Times(1).WillOnce([](int const fd_off) {
        EXPECT_EQ(start_idx, fd_off);
        // Return 2 FDs here, maybe it's another RAID0 device?
        return std::list< int >{INT_MAX - 2, INT_MAX - 3};
    });
    EXPECT_CALL(*device_b, open_for_uring(_)).Times(1).WillOnce([](int const fd_off) {
        // Device A took 2 uring offsets
        EXPECT_EQ(start_idx + 2, fd_off);
        return std::list< int >{INT_MAX - 1};
    });

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});
    auto fd_list = raid_device.open_for_uring(2);
    EXPECT_EQ(3, fd_list.size());
    EXPECT_NE(fd_list.end(), std::find(fd_list.begin(), fd_list.end(), (INT_MAX - 3)));
    EXPECT_NE(fd_list.end(), std::find(fd_list.begin(), fd_list.end(), (INT_MAX - 2)));
    EXPECT_NE(fd_list.end(), std::find(fd_list.begin(), fd_list.end(), (INT_MAX - 1)));

    EXPECT_CALL(*device_a, collect_async(_, _)).Times(0);
    EXPECT_CALL(*device_b, collect_async(_, _)).Times(0);
    std::list< ublkpp::async_result > result_list;
    raid_device.collect_async(nullptr, result_list);
    ASSERT_EQ(0, result_list.size());

    device_a->uses_ublk_iouring = false;
    device_b->uses_ublk_iouring = false;

    EXPECT_CALL(*device_a, collect_async(_, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, std::list< ublkpp::async_result >& compls) {
            compls.push_back(ublkpp::async_result{nullptr, 0, 5});
        });
    EXPECT_CALL(*device_b, collect_async(_, _))
        .Times(1)
        .WillOnce([](ublksrv_queue const*, std::list< ublkpp::async_result >& compls) {
            compls.push_back(ublkpp::async_result{nullptr, 1, 10});
        });

    raid_device.collect_async(nullptr, result_list);

    ASSERT_EQ(2, result_list.size());
    EXPECT_EQ(0, result_list.begin()->sub_cmd);
    EXPECT_EQ(5, result_list.begin()->result);
    EXPECT_EQ(1, (++result_list.begin())->sub_cmd);
    EXPECT_EQ(10, (++result_list.begin())->result);

    EXPECT_CALL(*device_a, handle_internal(_, _, _, _, _, _, _)).Times(1);
    EXPECT_CALL(*device_b, handle_internal(_, _, _, _, _, _, _)).Times(0);
    auto ublk_data = make_io_data(UBLK_IO_OP_WRITE, 120 * Ki, 64 * Ki);
    auto const internal_route = ublkpp::set_flags(ublkpp::sub_cmd_t{0b10000000}, ublkpp::sub_cmd_flags::INTERNAL);
    raid_device.queue_internal_resp(nullptr, &ublk_data, internal_route, 0);
    remove_io_data(ublk_data);

    EXPECT_CALL(*device_a, idle_transition(nullptr, true)).Times(1);
    EXPECT_CALL(*device_b, idle_transition(nullptr, true)).Times(1);
    raid_device.idle_transition(nullptr, true);

    EXPECT_CALL(*device_a, idle_transition(nullptr, false)).Times(1);
    EXPECT_CALL(*device_b, idle_transition(nullptr, false)).Times(1);
    raid_device.idle_transition(nullptr, false);
}
