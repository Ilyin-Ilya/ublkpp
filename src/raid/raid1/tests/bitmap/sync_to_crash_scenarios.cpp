#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "tests/test_disk.hpp"
#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/raid1_superblock.hpp"
#include "raid/raid1/tests/test_raid1_common.hpp"

using ::testing::_;
using ::testing::Return;

// Test crash during multi-page batch write
TEST(Raid1BitmapCrash, CrashDuringBatchWrite) {
    // Limit max_io to 8 KiB = 2 pages max per batch
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{
        .capacity = 16 * ublkpp::Gi,
        .max_io = 8 * ublkpp::Ki
    });
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 5 consecutive pages (will create 3 batches: 2, 2, 1)
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0, 5 * page_width);

    // Simulate crash: first batch succeeds (2 pages), second batch fails (2 pages)
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            // First batch writes successfully
            EXPECT_EQ(2U, nr_vecs); // Max 2 pages per batch
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t) -> ublkpp::io_result {
            // Second batch fails (simulates crash/power failure)
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());

    // Should propagate failure
    EXPECT_FALSE(res);
    EXPECT_EQ(std::errc::io_error, res.error());
}

// Test partial write in the middle of batching
TEST(Raid1BitmapCrash, PartialBatchWrite) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 16 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty pages: 0-2 (batch 1), gap, 5-7 (batch 2)
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0 * page_width, 3 * page_width);
    bitmap.dirty_region(5 * page_width, 3 * page_width);

    int call_count = 0;
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            call_count++;
            // First batch succeeds
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([&call_count](uint8_t, iovec*, uint32_t, off_t) -> ublkpp::io_result {
            call_count++;
            // Second batch fails
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());

    EXPECT_FALSE(res);
    EXPECT_EQ(2, call_count); // Both batches attempted
}

// Test recovery after failed sync_to (clean_unmount should be 0)
TEST(Raid1BitmapCrash, RecoveryAfterFailedSync) {
    // This simulates the full shutdown/reboot cycle

    // Setup: Device with initial bitmap
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf1 = make_test_superbitmap();
    auto bitmap1 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf1.get());

    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap1.dirty_region(0, 2 * page_width); // Dirty 2 pages

    // Simulate crash during sync_to
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t) -> ublkpp::io_result {
            // Write fails (crash)
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto sync_res = bitmap1.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_FALSE(sync_res); // Sync failed

    // At this point, clean_unmount would NOT be set to 1 in the real code
    // because sync_to failed and execution returned early

    // On reboot, load_from would not be called (full resync triggered instead)
    // because clean_unmount == 0 triggers the condition in raid1.cpp:171
}

// Test that batch size respects device limits
TEST(Raid1BitmapCrash, BatchSizeWithDeviceLimits) {
    // Device with very small max_io
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{
        .capacity = 16 * ublkpp::Gi,
        .max_io = 8 * ublkpp::Ki // 8 KiB = 2 pages max
    });
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 6 consecutive pages (should be split into batches of max 2)
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0, 6 * page_width);

    // Should result in 3 batches of 2 pages each
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            EXPECT_LE(nr_vecs, 2U); // Max 2 pages per batch
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test failure in the middle batch (first succeeds, middle fails, last not attempted)
TEST(Raid1BitmapCrash, FailureInMiddleBatch) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{
        .capacity = 32 * ublkpp::Gi,
        .max_io = 8 * ublkpp::Ki // Max 2 pages per batch
    });
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(32 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 6 consecutive pages (will create 3 batches of 2 pages each)
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0, 6 * page_width);

    int call_count = 0;
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2) // Only 2 batches attempted (third not reached due to failure)
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(2U, nr_vecs); // First batch: pages 0-1
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([&call_count](uint8_t, iovec*, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(2U, nr_vecs); // Second batch: pages 2-3
            // Fail here (simulates crash)
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());

    EXPECT_FALSE(res);
    EXPECT_EQ(2, call_count); // Third batch never attempted
}

// Test sync_to with offset parameter (for SuperBlock)
TEST(Raid1BitmapCrash, SyncToWithOffset) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0, page_width);

    constexpr auto SUPERBLOCK_SIZE = 4096UL; // sizeof(SuperBlock)

    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([SUPERBLOCK_SIZE](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            EXPECT_EQ(1U, nr_vecs);
            // Verify offset is after SuperBlock
            EXPECT_EQ(SUPERBLOCK_SIZE, addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, SUPERBLOCK_SIZE);
    EXPECT_TRUE(res);
}

// Test that consecutive page detection works correctly
TEST(Raid1BitmapCrash, ConsecutivePageDetection) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 16 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;

    // Create pattern: pages 0,1,2, gap, pages 4,5, gap, page 7
    bitmap.dirty_region(0 * page_width, 3 * page_width);
    bitmap.dirty_region(4 * page_width, 2 * page_width);
    bitmap.dirty_region(7 * page_width, page_width);

    // Should result in 3 separate batch writes
    int call_count = 0;
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(3U, nr_vecs); // Pages 0,1,2
            EXPECT_EQ(ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(2U, nr_vecs); // Pages 4,5
            EXPECT_EQ(5 * ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(1U, nr_vecs); // Page 7
            EXPECT_EQ(8 * ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
    EXPECT_EQ(3, call_count);
}
