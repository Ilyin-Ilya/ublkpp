#include "test_raid1_common.hpp"

// Brief: Test that RAID1 devices correctly report paramaters based on Devices A and B
//
// Construct a RAID1 device with Identical underlying devices that match on every
// parameter. The final RAID1 parameters should be equivalent to the underlying
// devices themselves.
TEST(Raid1, IdenticalDeviceProbing) {
    auto device_a = CREATE_DISK_A(TestParams{.capacity = Gi});
    auto device_b = CREATE_DISK_B(TestParams{.capacity = Gi});

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
    EXPECT_EQ(raid_device.capacity(), (Gi)-raid_device.reserved_size()); // 504KiB is for alignment!
    EXPECT_STREQ(raid_device.id().c_str(), "RAID1");

    // CanDiscard and DirectIO `true` be default.
    EXPECT_EQ(raid_device.can_discard(), true);
    EXPECT_EQ(raid_device.direct_io, true);

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
