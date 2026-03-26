#include "test_raid1_common.hpp"

// Brief: Test that RAID1 array rejects devices larger than SuperBitmap can track
// The SuperBitmap can track at most 32,176 bitmap pages (k_superbitmap_bits)
// With 32KiB chunks, each bitmap page covers 1 GiB, giving a max capacity of ~31.4 TiB
//
TEST(Raid1, DevicesLargerThanAllowed) {
    // Create disks without SuperBlock read/write expectations since we'll throw before those operations
    auto device_a = CREATE_DISK_F(TestParams{.capacity = UINT64_MAX}, false, true, false, true, false);
    auto device_b = CREATE_DISK_F(TestParams{.capacity = UINT64_MAX}, true, true, false, true, false);

    // Should throw exception for devices exceeding SuperBitmap capacity
    EXPECT_THROW(
        ublkpp::Raid1Disk(boost::uuids::string_generator()(test_uuid), device_a, device_b),
        std::runtime_error
    );
}
