#include "test_raid0_common.hpp"

// Brief: Test that RAID0 devices correctly report paramaters based on Devices A and B
//
// Construct a RAID0 device with Differing underlying devices that deviate on every
// parameter. The final RAID0 parameters should represent the lowest feature set of
// both devices including Capacity, BlockSize, Discard and DirectI/O support.
TEST(Raid0, DiffereingDeviceProbing) {
    auto device_a = CREATE_DISK((TestParams{.capacity = 5 * Gi, .l_size = 512, .p_size = 8 * Ki, .direct_io = false}));
    auto device_b =
        CREATE_DISK((TestParams{.capacity = 3 * Gi, .l_size = 4 * Ki, .p_size = 4 * Ki, .can_discard = false}));

    auto raid_device = ublkpp::Raid0Disk(boost::uuids::random_generator()(), 32 * Ki,
                                         std::vector< std::shared_ptr< UblkDisk > >{device_a, device_b});
    // Smallest disk was 3GiB, so 2 * 3GiB
    EXPECT_EQ(raid_device.capacity(), (6 * Gi) - (512 * Ki));

    EXPECT_EQ(raid_device.block_size(), 4 * Ki);

    // PBS is the stripe size for RAID-0
    EXPECT_EQ(raid_device.params()->basic.physical_bs_shift, ilog2(32 * Ki));

    // Device B lacks Discard support
    EXPECT_EQ(raid_device.can_discard(), false);
    // Device A lacks DirectI/O support
    EXPECT_EQ(raid_device.direct_io, false);
}
