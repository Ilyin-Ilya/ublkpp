#include "test_raid1_common.hpp"

// Brief: Test that RAID1 devices correctly report paramaters based on Devices A and B
//
// Construct a RAID1 device with Differing underlying devices that deviate on every
// parameter. The final RAID1 parameters should represent the lowest feature set of
// both devices including Capacity, BlockSize, Discard
TEST(Raid1, DiffereingDeviceProbing) {
    auto device_a = CREATE_DISK_A((TestParams{.capacity = 5 * Gi, .l_size = 512, .p_size = 8 * Ki}));
    auto device_b =
        CREATE_DISK_B((TestParams{.capacity = 3 * Gi, .l_size = 4 * Ki, .p_size = 4 * Ki, .can_discard = false}));

    auto raid_device = ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b);
    // Smallest disk was 3GiB
    EXPECT_EQ(raid_device.capacity(), (3 * Gi) - raid_device.reserved_size());

    // LBS/PBS represent by shift size, not raw byte count
    EXPECT_EQ(raid_device.block_size(), 4 * Ki);
    EXPECT_EQ(raid_device.params()->basic.physical_bs_shift, ilog2(8 * Ki));

    // Device B lacks Discard support
    EXPECT_EQ(raid_device.can_discard(), false);

    // expect unmount_clean update
    EXPECT_TO_WRITE_SB(device_a);
    EXPECT_TO_WRITE_SB(device_b);
}
