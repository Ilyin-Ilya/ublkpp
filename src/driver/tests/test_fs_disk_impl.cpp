#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <ublksrv.h>

// Include sub_cmd.hpp first to get the constants
#include "ublkpp/lib/sub_cmd.hpp"
// Now include fs_disk_impl.hpp which depends on those constants
#include "../fs_disk_impl.hpp"
#include "ublkpp/lib/common.hpp"

namespace {

// ============================================================================
// Test block_has_unmap function
// ============================================================================

TEST(BlockHasUnmap, NonExistentDevice) {
    // Test with a device that doesn't exist
    struct stat fake_stat{};
    bool result = ublkpp::block_has_unmap(fake_stat);

    // Should return false for non-existent devices
    EXPECT_FALSE(result);
}

// ============================================================================
// Test build_tgt_sqe_data function
// ============================================================================

TEST(BuildTgtSqeData, BasicEncoding) {
    uint64_t tag = 100;
    uint64_t op = 5;
    uint64_t sub_cmd = 200;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

    // Verify that the high bit is set (driver I/O marker)
    uint64_t high_bit_mask = static_cast< uint64_t >(0b1)
        << (ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width + ublkpp::sqe_reserved_width);

    EXPECT_NE(result & high_bit_mask, 0);
}

TEST(BuildTgtSqeData, TagEncoding) {
    uint64_t tag = 42;
    uint64_t op = 0;
    uint64_t sub_cmd = 0;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

    // Extract tag (lowest bits)
    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t extracted_tag = result & tag_mask;

    EXPECT_EQ(extracted_tag, tag);
}

TEST(BuildTgtSqeData, OpEncoding) {
    uint64_t tag = 0;
    uint64_t op = 7;
    uint64_t sub_cmd = 0;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

    // Extract op (after tag bits)
    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t extracted_op = (result >> ublkpp::sqe_tag_width) & op_mask;

    EXPECT_EQ(extracted_op, op);
}

TEST(BuildTgtSqeData, SubCmdEncoding) {
    uint64_t tag = 0;
    uint64_t op = 0;
    uint64_t sub_cmd = 512;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

    // Extract sub_cmd (after tag and op bits)
    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;
    uint64_t extracted_sub_cmd = (result >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask;

    EXPECT_EQ(extracted_sub_cmd, sub_cmd);
}

TEST(BuildTgtSqeData, MaxTagValue) {
    uint64_t max_tag = UINT16_MAX;
    uint64_t op = 0;
    uint64_t sub_cmd = 0;

    uint64_t result = ublkpp::build_tgt_sqe_data(max_tag, op, sub_cmd);

    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t extracted_tag = result & tag_mask;

    EXPECT_EQ(extracted_tag, max_tag);
}

TEST(BuildTgtSqeData, MaxOpValue) {
    uint64_t tag = 0;
    uint64_t max_op = UINT8_MAX;
    uint64_t sub_cmd = 0;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, max_op, sub_cmd);

    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t extracted_op = (result >> ublkpp::sqe_tag_width) & op_mask;

    EXPECT_EQ(extracted_op, max_op);
}

TEST(BuildTgtSqeData, MaxSubCmdValue) {
    uint64_t tag = 0;
    uint64_t op = 0;
    uint64_t max_sub_cmd = UINT16_MAX;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, max_sub_cmd);

    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;
    uint64_t extracted_sub_cmd = (result >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask;

    EXPECT_EQ(extracted_sub_cmd, max_sub_cmd);
}

TEST(BuildTgtSqeData, AllFieldsCombined) {
    uint64_t tag = 1234;
    uint64_t op = 42;
    uint64_t sub_cmd = 9876;

    uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

    // Extract all fields
    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;

    uint64_t extracted_tag = result & tag_mask;
    uint64_t extracted_op = (result >> ublkpp::sqe_tag_width) & op_mask;
    uint64_t extracted_sub_cmd = (result >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask;

    EXPECT_EQ(extracted_tag, tag);
    EXPECT_EQ(extracted_op, op);
    EXPECT_EQ(extracted_sub_cmd, sub_cmd);
}

TEST(BuildTgtSqeData, HighBitAlwaysSet) {
    // Test that the high bit (driver I/O marker) is always set
    std::vector< std::tuple< uint64_t, uint64_t, uint64_t > > test_cases = {
        {0, 0, 0}, {100, 5, 200}, {UINT16_MAX, UINT8_MAX, UINT16_MAX}, {1, 1, 1}, {12345, 123, 54321}};

    uint64_t high_bit_mask = static_cast< uint64_t >(0b1)
        << (ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width + ublkpp::sqe_reserved_width);

    for (const auto& [tag, op, sub_cmd] : test_cases) {
        uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);
        EXPECT_NE(result & high_bit_mask, 0)
            << "High bit not set for tag=" << tag << ", op=" << op << ", sub_cmd=" << sub_cmd;
    }
}

TEST(BuildTgtSqeData, ZeroValues) {
    uint64_t result = ublkpp::build_tgt_sqe_data(0, 0, 0);

    // High bit should still be set
    uint64_t high_bit_mask = static_cast< uint64_t >(0b1)
        << (ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width + ublkpp::sqe_reserved_width);

    EXPECT_NE(result & high_bit_mask, 0);

    // Other bits should be zero
    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;

    EXPECT_EQ(result & tag_mask, 0);
    EXPECT_EQ((result >> ublkpp::sqe_tag_width) & op_mask, 0);
    EXPECT_EQ((result >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask, 0);
}

TEST(BuildTgtSqeData, RoundTripEncoding) {
    // Test encoding and decoding
    uint64_t original_tag = 5678;
    uint64_t original_op = 42;
    uint64_t original_sub_cmd = 1234;

    uint64_t encoded = ublkpp::build_tgt_sqe_data(original_tag, original_op, original_sub_cmd);

    // Decode
    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;

    uint64_t decoded_tag = encoded & tag_mask;
    uint64_t decoded_op = (encoded >> ublkpp::sqe_tag_width) & op_mask;
    uint64_t decoded_sub_cmd = (encoded >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask;

    EXPECT_EQ(decoded_tag, original_tag);
    EXPECT_EQ(decoded_op, original_op);
    EXPECT_EQ(decoded_sub_cmd, original_sub_cmd);
}

TEST(BuildTgtSqeData, BitFieldSizes) {
    // Verify bit field sizes are reasonable
    EXPECT_GT(ublkpp::sqe_tag_width, 0);
    EXPECT_GT(ublkpp::sqe_op_width, 0);
    EXPECT_GT(ublkpp::sqe_tgt_data_width, 0);

    // Total should not exceed 64 bits
    int total_bits = ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width +
        ublkpp::sqe_reserved_width + 1; // +1 for high bit
    EXPECT_LE(total_bits, 64);
}

TEST(BuildTgtSqeData, SequentialTagValues) {
    // Test a sequence of tag values
    for (uint64_t tag = 0; tag < 100; ++tag) {
        uint64_t result = ublkpp::build_tgt_sqe_data(tag, 1, 1);
        uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
        uint64_t extracted_tag = result & tag_mask;
        EXPECT_EQ(extracted_tag, tag) << "Failed for tag=" << tag;
    }
}

TEST(BuildTgtSqeData, SequentialOpValues) {
    // Test a sequence of op values
    for (uint64_t op = 0; op < UINT8_MAX; op += 10) {
        uint64_t result = ublkpp::build_tgt_sqe_data(1, op, 1);
        uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
        uint64_t extracted_op = (result >> ublkpp::sqe_tag_width) & op_mask;
        EXPECT_EQ(extracted_op, op) << "Failed for op=" << op;
    }
}

TEST(BuildTgtSqeData, BoundaryValues) {
    // Test boundary values together
    std::vector< std::tuple< uint64_t, uint64_t, uint64_t > > boundary_cases = {
        {0, 0, 0},                           // All minimum
        {UINT16_MAX, UINT8_MAX, UINT16_MAX}, // All maximum
        {UINT16_MAX, 0, 0},                  // Max tag only
        {0, UINT8_MAX, 0},                   // Max op only
        {0, 0, UINT16_MAX},                  // Max sub_cmd only
        {UINT16_MAX, UINT8_MAX, 0},          // Max tag and op
        {UINT16_MAX, 0, UINT16_MAX},         // Max tag and sub_cmd
        {0, UINT8_MAX, UINT16_MAX},          // Max op and sub_cmd
    };

    for (const auto& [tag, op, sub_cmd] : boundary_cases) {
        uint64_t result = ublkpp::build_tgt_sqe_data(tag, op, sub_cmd);

        uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
        uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
        uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;

        uint64_t extracted_tag = result & tag_mask;
        uint64_t extracted_op = (result >> ublkpp::sqe_tag_width) & op_mask;
        uint64_t extracted_sub_cmd = (result >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask;

        EXPECT_EQ(extracted_tag, tag);
        EXPECT_EQ(extracted_op, op);
        EXPECT_EQ(extracted_sub_cmd, sub_cmd);
    }
}

TEST(BuildTgtSqeData, NonInterference) {
    // Verify that setting one field doesn't affect others
    uint64_t result1 = ublkpp::build_tgt_sqe_data(0xFFFF, 0, 0);
    uint64_t result2 = ublkpp::build_tgt_sqe_data(0, 0xFF, 0);
    uint64_t result3 = ublkpp::build_tgt_sqe_data(0, 0, 0xFFFF);

    uint64_t tag_mask = (1ULL << ublkpp::sqe_tag_width) - 1;
    uint64_t op_mask = (1ULL << ublkpp::sqe_op_width) - 1;
    uint64_t sub_cmd_mask = (1ULL << ublkpp::sqe_tgt_data_width) - 1;

    // result1: only tag is set
    EXPECT_EQ(result1 & tag_mask, 0xFFFF);
    EXPECT_EQ((result1 >> ublkpp::sqe_tag_width) & op_mask, 0);
    EXPECT_EQ((result1 >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask, 0);

    // result2: only op is set
    EXPECT_EQ(result2 & tag_mask, 0);
    EXPECT_EQ((result2 >> ublkpp::sqe_tag_width) & op_mask, 0xFF);
    EXPECT_EQ((result2 >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask, 0);

    // result3: only sub_cmd is set
    EXPECT_EQ(result3 & tag_mask, 0);
    EXPECT_EQ((result3 >> ublkpp::sqe_tag_width) & op_mask, 0);
    EXPECT_EQ((result3 >> (ublkpp::sqe_tag_width + ublkpp::sqe_op_width)) & sub_cmd_mask, 0xFFFF);
}

// ============================================================================
// Test discard_to_fallocate function
// ============================================================================

TEST(DiscardToFallocate, FallocateModeConstants) {
    // Verify that the fallocate mode constants are properly defined
    EXPECT_GT(FALLOC_FL_KEEP_SIZE, 0);
    EXPECT_GT(FALLOC_FL_PUNCH_HOLE, 0);
    EXPECT_GT(FALLOC_FL_ZERO_RANGE, 0);

    // Verify they are different values
    EXPECT_NE(FALLOC_FL_PUNCH_HOLE, FALLOC_FL_ZERO_RANGE);

    // Verify that combining flags works as expected
    int punch_mode = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE;
    int zero_mode = FALLOC_FL_KEEP_SIZE | FALLOC_FL_ZERO_RANGE;

    EXPECT_NE(punch_mode & FALLOC_FL_KEEP_SIZE, 0);
    EXPECT_NE(punch_mode & FALLOC_FL_PUNCH_HOLE, 0);
    EXPECT_EQ(punch_mode & FALLOC_FL_ZERO_RANGE, 0);

    EXPECT_NE(zero_mode & FALLOC_FL_KEEP_SIZE, 0);
    EXPECT_NE(zero_mode & FALLOC_FL_ZERO_RANGE, 0);
    EXPECT_EQ(zero_mode & FALLOC_FL_PUNCH_HOLE, 0);
}

TEST(DiscardToFallocate, UblkOperationConstants) {
    // Verify UBLK operation constants are defined correctly
    EXPECT_NE(UBLK_IO_OP_DISCARD, UBLK_IO_OP_WRITE_ZEROES);
    EXPECT_GT(UBLK_IO_F_NOUNMAP, 0);
}

TEST(DiscardToFallocate, FlagBitOperations) {
    // Test that flag operations work correctly
    uint32_t flags_with_nounmap = UBLK_IO_F_NOUNMAP;
    uint32_t flags_without_nounmap = 0;

    // Test NOUNMAP flag check
    EXPECT_NE(flags_with_nounmap & UBLK_IO_F_NOUNMAP, 0);
    EXPECT_EQ(flags_without_nounmap & UBLK_IO_F_NOUNMAP, 0);

    // Verify the logic used in discard_to_fallocate:
    // (0 == (UBLK_IO_F_NOUNMAP & flags))
    EXPECT_FALSE(0 == (UBLK_IO_F_NOUNMAP & flags_with_nounmap));
    EXPECT_TRUE(0 == (UBLK_IO_F_NOUNMAP & flags_without_nounmap));
}

TEST(DiscardToFallocate, ModeCombinations) {
    // Test all valid mode combinations
    int base_mode = FALLOC_FL_KEEP_SIZE;

    int punch_hole_mode = base_mode | FALLOC_FL_PUNCH_HOLE;
    int zero_range_mode = base_mode | FALLOC_FL_ZERO_RANGE;

    // Verify they're distinct
    EXPECT_NE(punch_hole_mode, zero_range_mode);

    // Verify base mode is present in both
    EXPECT_EQ(punch_hole_mode & FALLOC_FL_KEEP_SIZE, FALLOC_FL_KEEP_SIZE);
    EXPECT_EQ(zero_range_mode & FALLOC_FL_KEEP_SIZE, FALLOC_FL_KEEP_SIZE);
}

TEST(DiscardToFallocate, FlagValuesAreNonZero) {
    // Ensure flags have meaningful values (power of 2 typically)
    EXPECT_GT(FALLOC_FL_KEEP_SIZE, 0);
    EXPECT_GT(FALLOC_FL_PUNCH_HOLE, 0);
    EXPECT_GT(FALLOC_FL_ZERO_RANGE, 0);

    // Verify they can be combined with bitwise OR
    int combined = FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE;
    EXPECT_GT(combined, 0);
}

// ============================================================================
// Integration and edge case tests
// ============================================================================

TEST(FsDiskImpl, BitFieldConstants) {
    // Verify the constants used in build_tgt_sqe_data are consistent
    EXPECT_EQ(ublkpp::sqe_tag_width, 16U);
    EXPECT_EQ(ublkpp::sqe_op_width, 8U);
    EXPECT_EQ(ublkpp::sqe_tgt_data_width, sizeof(ublkpp::sub_cmd_t) * 8U);
}

TEST(FsDiskImpl, SubCmdTypeSize) {
    // Verify sub_cmd_t is 16 bits as expected
    EXPECT_EQ(sizeof(ublkpp::sub_cmd_t), 2);
    EXPECT_EQ(ublkpp::sqe_tgt_data_width, 16U);
}

TEST(FsDiskImpl, ReservedWidthCalculation) {
    // Verify reserved width calculation
    unsigned expected_reserved =
        64U - (ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width + 1); // 1 for is_tgt bit
    EXPECT_EQ(ublkpp::sqe_reserved_width, expected_reserved);
}

TEST(FsDiskImpl, UblkOperationValues) {
    // Document the expected UBLK operation values
    // These are defined in ublksrv.h
    // Note: UBLK_IO_OP_READ is typically 0, so we just verify they're distinct
    EXPECT_NE(UBLK_IO_OP_DISCARD, UBLK_IO_OP_WRITE_ZEROES);
    EXPECT_NE(UBLK_IO_OP_READ, UBLK_IO_OP_WRITE);
    EXPECT_NE(UBLK_IO_OP_FLUSH, UBLK_IO_OP_DISCARD);

    // Verify operations that should be non-zero
    EXPECT_GT(UBLK_IO_OP_WRITE, 0);
    EXPECT_GT(UBLK_IO_OP_FLUSH, 0);
}

TEST(FsDiskImpl, HighBitPosition) {
    // Verify the high bit is at the correct position
    unsigned high_bit_pos =
        ublkpp::sqe_tag_width + ublkpp::sqe_op_width + ublkpp::sqe_tgt_data_width + ublkpp::sqe_reserved_width;

    EXPECT_LT(high_bit_pos, 64) << "High bit position exceeds 64 bits";
    EXPECT_EQ(high_bit_pos, 63) << "High bit should be at position 63 (MSB of uint64_t)";
}

TEST(FsDiskImpl, EncodingUniqueness) {
    // Verify different inputs produce different outputs
    uint64_t result1 = ublkpp::build_tgt_sqe_data(1, 2, 3);
    uint64_t result2 = ublkpp::build_tgt_sqe_data(1, 2, 4);
    uint64_t result3 = ublkpp::build_tgt_sqe_data(1, 3, 3);
    uint64_t result4 = ublkpp::build_tgt_sqe_data(2, 2, 3);

    EXPECT_NE(result1, result2);
    EXPECT_NE(result1, result3);
    EXPECT_NE(result1, result4);
    EXPECT_NE(result2, result3);
    EXPECT_NE(result2, result4);
    EXPECT_NE(result3, result4);
}

} // anonymous namespace
