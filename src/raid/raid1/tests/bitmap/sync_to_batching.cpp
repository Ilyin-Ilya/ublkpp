#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "tests/test_disk.hpp"
#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/super_bitmap.hpp"
#include "raid/raid1/raid1_superblock.hpp"
#include "raid/raid1/tests/test_raid1_common.hpp"

using ::testing::_;
using ::testing::Return;

// Test that sync_to batches consecutive pages into single write operations
TEST(Raid1Bitmap, SyncToBatchesConsecutivePages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 4 consecutive pages worth of data
    bitmap.dirty_region(0, 4 * 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8); // 4 pages

    // Expect SINGLE batched write for all 4 consecutive pages
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            EXPECT_EQ(4U, nr_vecs); // 4 pages batched together
            EXPECT_EQ(4 * ublkpp::raid1::Bitmap::page_size(),
                     ublkpp::__iovec_len(iovecs, iovecs + nr_vecs));
            EXPECT_EQ(ublkpp::raid1::Bitmap::page_size(), addr); // Starts after SuperBlock
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test that sync_to creates separate writes for non-consecutive pages
TEST(Raid1Bitmap, SyncToSeparatesNonConsecutivePages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty page 0 and page 2 (non-consecutive)
    bitmap.dirty_region(0, 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8); // Page 0
    bitmap.dirty_region(2 * 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8,
                       32 * ublkpp::Ki * 4 * ublkpp::Ki * 8); // Page 2

    // Expect 2 separate writes (not batched due to gap)
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            EXPECT_EQ(1U, nr_vecs); // First page alone
            EXPECT_EQ(ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            EXPECT_EQ(1U, nr_vecs); // Second page alone
            EXPECT_EQ(3 * ublkpp::raid1::Bitmap::page_size(), addr); // Page 2 is at offset 3
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test that sync_to respects max_pages_per_tx batch limit
TEST(Raid1Bitmap, SyncToRespectsMaxBatchSize) {
    // Create device with small max_io to force batching limits
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{
        .capacity = 16 * ublkpp::Gi,
        .max_io = 4 * ublkpp::Ki // 4 KiB = 1 page, so max_batch = 1
    });
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 3 consecutive pages
    bitmap.dirty_region(0, 3 * 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8);

    // With max_batch = 1, expect 3 separate writes even though pages are consecutive
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(3)
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            EXPECT_EQ(1U, nr_vecs); // Max 1 page per batch
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test that sync_to skips pages loaded from disk (not modified)
TEST(Raid1Bitmap, SyncToSkipsUnmodifiedLoadedPages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();

    // Mark all 8 pages as dirty in the SuperBitmap so load_from will load them
    auto sb = ublkpp::raid1::SuperBitmap(superbitmap_buf.get());
    for (int i = 0; i < 8; ++i) sb.set_bit(i);

    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Load bitmap from device (simulates reboot scenario)
    // 8 GiB capacity with 32 KiB chunks = 8 bitmap pages (1 GiB per page)
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(8)
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t, off_t) -> ublkpp::io_result {
            memset(iovecs->iov_base, 0xff, iovecs->iov_len); // Non-zero page
            return ublkpp::raid1::Bitmap::page_size();
        });

    bitmap.load_from(*device);

    // Now try to sync_to - should NOT write loaded pages (they're unmodified)
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(0); // No writes expected!

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test that sync_to writes pages that were loaded then modified
TEST(Raid1Bitmap, SyncToWritesModifiedLoadedPages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();

    // Mark all 8 pages as dirty in the SuperBitmap so load_from will load them
    auto sb = ublkpp::raid1::SuperBitmap(superbitmap_buf.get());
    for (int i = 0; i < 8; ++i) sb.set_bit(i);

    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Load bitmap from device
    // 8 GiB capacity with 32 KiB chunks = 8 bitmap pages (1 GiB per page)
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(8)
        .WillRepeatedly([](uint8_t, iovec* iovecs, uint32_t, off_t) -> ublkpp::io_result {
            memset(iovecs->iov_base, 0xff, iovecs->iov_len);
            return ublkpp::raid1::Bitmap::page_size();
        });

    bitmap.load_from(*device);

    // Now modify a loaded page by dirtying a new region
    bitmap.dirty_region(0, 32 * ublkpp::Ki); // Modifies page 0

    // Should write the modified page
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t) -> ublkpp::io_result {
            EXPECT_EQ(1U, nr_vecs);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}

// Test that sync_to handles write failures correctly
TEST(Raid1Bitmap, SyncToHandlesWriteFailure) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty 2 consecutive pages
    bitmap.dirty_region(0, 2 * 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8);

    // First write fails
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillOnce([](uint8_t, iovec*, uint32_t, off_t) -> ublkpp::io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_FALSE(res);
    EXPECT_EQ(std::errc::io_error, res.error());
}

// Test batching with mixed consecutive and non-consecutive pages
TEST(Raid1Bitmap, SyncToBatchesMixedPages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 16 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(16 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Dirty pages: 0, 1, 2 (consecutive), then 5, 6 (consecutive)
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    bitmap.dirty_region(0 * page_width, page_width);       // Page 0
    bitmap.dirty_region(1 * page_width, page_width);       // Page 1
    bitmap.dirty_region(2 * page_width, page_width);       // Page 2
    // Gap at page 3 and 4
    bitmap.dirty_region(5 * page_width, page_width);       // Page 5
    bitmap.dirty_region(6 * page_width, page_width);       // Page 6

    int call_count = 0;
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(2)
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(3U, nr_vecs); // First batch: pages 0, 1, 2
            EXPECT_EQ(ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        })
        .WillOnce([&call_count](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            call_count++;
            EXPECT_EQ(2U, nr_vecs); // Second batch: pages 5, 6
            EXPECT_EQ(6 * ublkpp::raid1::Bitmap::page_size(), addr);
            return ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
        });

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
    EXPECT_EQ(2, call_count);
}

// Test empty bitmap sync_to (no dirty pages)
TEST(Raid1Bitmap, SyncToEmptyBitmap) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    // Don't dirty anything

    // Should not write anything
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(0);

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res); // Returns success for empty bitmap
}

// Test that sync_to skips zero pages (cleaned pages)
TEST(Raid1Bitmap, SyncToSkipsZeroPages) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, superbitmap_buf.get());

    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;

    // Dirty then clean a region (should become zero page)
    bitmap.dirty_region(0, page_width);
    // Clean it back (this should zero out the page)
    [[maybe_unused]] auto [page, pg_off, sz] = bitmap.clean_region(0, page_width);

    // Should not write zero pages
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(0);

    auto res = bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(res);
}
