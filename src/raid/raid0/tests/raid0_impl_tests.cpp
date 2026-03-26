#include "test_raid0_common.hpp"

// ============================================================================
// Tests for raid0_impl.hpp helper functions
// ============================================================================

// Test: next_subcmd with single disk (stride_width == stripe_size)
TEST(Raid0Impl, NextSubcmdSingleDisk) {
    // With single disk, no striping needed - passthrough
    uint32_t const stride_width = 128 * Ki;
    uint32_t const stripe_size = 128 * Ki; // Same as stride_width

    auto [device_off, logical_off, sz] = ublkpp::raid0::next_subcmd(stride_width, stripe_size, 0, 4 * Ki);

    EXPECT_EQ(device_off, 0);
    EXPECT_EQ(logical_off, 0);
    EXPECT_EQ(sz, 4 * Ki);

    // Test with different offset
    auto [device_off2, logical_off2, sz2] = ublkpp::raid0::next_subcmd(stride_width, stripe_size, 64 * Ki, 8 * Ki);

    EXPECT_EQ(device_off2, 0);
    EXPECT_EQ(logical_off2, 64 * Ki);
    EXPECT_EQ(sz2, 8 * Ki);
}

// Test: next_subcmd at stripe boundary
TEST(Raid0Impl, NextSubcmdAtBoundary) {
    // 3 disks, 32 KB stripe size
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Access exactly at stripe boundary
    auto [device_off, logical_off, sz] = ublkpp::raid0::next_subcmd(stride_width, stripe_size, 32 * Ki, 4 * Ki);

    // Should go to second device (offset 1), at the beginning
    EXPECT_EQ(device_off, 1);
    EXPECT_EQ(logical_off, 0);
    EXPECT_EQ(sz, 4 * Ki);
}

// Test: next_subcmd that spans to end of stripe
TEST(Raid0Impl, NextSubcmdToEndOfStripe) {
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Access that would exceed stripe boundary - should be clamped
    auto [device_off, logical_off, sz] = ublkpp::raid0::next_subcmd(stride_width, stripe_size, 30 * Ki, 8 * Ki);

    EXPECT_EQ(device_off, 0);
    EXPECT_EQ(logical_off, 30 * Ki);
    EXPECT_EQ(sz, 2 * Ki); // Clamped to stripe boundary
}

// Test: next_subcmd in middle of second stride
TEST(Raid0Impl, NextSubcmdSecondStride) {
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Address in second stride, third device
    uint64_t addr = (3 * 32 * Ki) + (2 * 32 * Ki) + (16 * Ki);

    auto [device_off, logical_off, sz] = ublkpp::raid0::next_subcmd(stride_width, stripe_size, addr, 4 * Ki);

    EXPECT_EQ(device_off, 2);                      // Third device
    EXPECT_EQ(logical_off, (32 * Ki) + (16 * Ki)); // Second chunk + offset
    EXPECT_EQ(sz, 4 * Ki);
}

// Test: merged_subcmds with single disk
TEST(Raid0Impl, MergedSubcmdsSingleDisk) {
    uint32_t const stride_width = 128 * Ki;
    uint32_t const stripe_size = 128 * Ki; // Single disk

    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, 64 * Ki);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].first, 0);        // offset
    EXPECT_EQ(result[0].second, 64 * Ki); // length
}

// Test: merged_subcmds that hits all devices once
TEST(Raid0Impl, MergedSubcmdsAllDevicesOnce) {
    // 3 disks, 32 KB stripe
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Access one full stride
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, stride_width);

    ASSERT_EQ(result.size(), 3);

    // Each device should get exactly one stripe
    EXPECT_EQ(result[0].first, 0);
    EXPECT_EQ(result[0].second, 32 * Ki);

    EXPECT_EQ(result[1].first, 0);
    EXPECT_EQ(result[1].second, 32 * Ki);

    EXPECT_EQ(result[2].first, 0);
    EXPECT_EQ(result[2].second, 32 * Ki);
}

// Test: merged_subcmds that wraps around (hits same device multiple times)
TEST(Raid0Impl, MergedSubcmdsWrapAround) {
    // 3 disks, 32 KB stripe
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Access 2 full strides - each device should be hit twice and merged
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, 2 * stride_width);

    ASSERT_EQ(result.size(), 3);

    // Each device should get two stripes merged
    for (auto const& [device_off, region] : result) {
        EXPECT_EQ(region.second, 64 * Ki); // 2 * 32 Ki
    }
}

// Test: merged_subcmds starting mid-stride
TEST(Raid0Impl, MergedSubcmdsMidStride) {
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Start in middle of first device, go to middle of second
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 16 * Ki, 24 * Ki);

    // Should hit device 0 (partial) and device 1 (partial)
    ASSERT_EQ(result.size(), 2);

    EXPECT_EQ(result[0].first, 16 * Ki);
    EXPECT_EQ(result[0].second, 16 * Ki); // Rest of first stripe

    EXPECT_EQ(result[1].first, 0);
    EXPECT_EQ(result[1].second, 8 * Ki); // Partial second stripe
}

// Test: merged_subcmds with complex wrapping pattern
TEST(Raid0Impl, MergedSubcmdsComplexWrapping) {
    uint32_t const stride_width = 4 * 16 * Ki; // 4 devices, 16 KB each
    uint32_t const stripe_size = 16 * Ki;

    // Access that wraps multiple times
    uint64_t addr = 8 * Ki;  // Middle of first stripe
    uint64_t len = 100 * Ki; // Spans multiple strides

    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, addr, len);

    ASSERT_EQ(result.size(), 4); // All 4 devices should be hit

    // Verify total length sums correctly
    uint64_t total_len = 0;
    for (auto const& [device_off, region] : result) {
        total_len += region.second;
    }
    EXPECT_EQ(total_len, len);
}

// Test: merged_subcmds merging verification
TEST(Raid0Impl, MergedSubcmdsMergingVerification) {
    uint32_t const stride_width = 2 * 32 * Ki; // 2 devices
    uint32_t const stripe_size = 32 * Ki;

    // Access that hits device 0 twice with gap (device 1 in between)
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, 3 * 32 * Ki);

    ASSERT_EQ(result.size(), 2);

    // Device 0 should have merged two non-contiguous accesses
    EXPECT_EQ(result[0].first, 0);
    EXPECT_EQ(result[0].second, 64 * Ki); // Two 32KB stripes merged

    // Device 1 should have one stripe
    EXPECT_EQ(result[1].first, 0);
    EXPECT_EQ(result[1].second, 32 * Ki);
}

// Test: merged_subcmds with very small access
TEST(Raid0Impl, MergedSubcmdsSmallAccess) {
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Very small access (512 bytes)
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, 512);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].first, 0);
    EXPECT_EQ(result[0].second, 512);
}

// Test: merged_subcmds exactly one stripe
TEST(Raid0Impl, MergedSubcmdsExactlyOneStripe) {
    uint32_t const stride_width = 3 * 32 * Ki;
    uint32_t const stripe_size = 32 * Ki;

    // Exactly one stripe
    auto result = ublkpp::raid0::merged_subcmds(stride_width, stripe_size, 0, 32 * Ki);

    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result[0].first, 0);
    EXPECT_EQ(result[0].second, 32 * Ki);
}

// Test: SuperBlock structure size
TEST(Raid0Impl, SuperBlockSize) {
    EXPECT_EQ(sizeof(ublkpp::raid0::SuperBlock), ublkpp::raid0::k_page_size);
    EXPECT_EQ(sizeof(ublkpp::raid0::SuperBlock), 4096);
}

// Test: SuperBlock field offsets
TEST(Raid0Impl, SuperBlockLayout) {
    ublkpp::raid0::SuperBlock sb{};

    // Verify fields are accessible
    sb.header.magic[0] = 0x55;
    sb.header.version = htobe16(1);
    sb.fields.stripe_off = 0;
    sb.fields.stripe_size = htobe32(128 * Ki);

    EXPECT_EQ(sb.header.magic[0], 0x55);
    EXPECT_EQ(be16toh(sb.header.version), 1);
    EXPECT_EQ(sb.fields.stripe_off, 0);
    EXPECT_EQ(be32toh(sb.fields.stripe_size), 128 * Ki);
}
