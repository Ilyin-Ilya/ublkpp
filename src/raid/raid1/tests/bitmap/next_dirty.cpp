#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/tests/test_raid1_common.hpp"

using ublkpp::Ki;

// Test the iteration through dirty pages
TEST(Raid1, NextDirty) {
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(100 * ublkpp::Gi, 32 * Ki, 4 * Ki, superbitmap_buf.get());
    bitmap.dirty_region(0x4096, 512 * Ki);
    bitmap.dirty_region(0x23f1000, 16 * Ki);
    bitmap.dirty_region(0x23f8000, 64 * Ki);
    bitmap.dirty_region(ublkpp::Gi - (4 * Ki), 8 * Ki);
    bitmap.dirty_region(ublkpp::Gi, 4 * Ki);
    bitmap.dirty_region(5 * ublkpp::Gi, 4 * Ki);
    EXPECT_EQ(3, bitmap.dirty_pages());
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(0, off);                      // Chunk aligned
        EXPECT_EQ((512 * Ki) + (32 * Ki), len); // Cross word dirty
        bitmap.clean_region(off, len);
    }
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(0x23f0000, off); // Chunk aligned
        EXPECT_EQ(64 * Ki, len);   // Merged dirty
        bitmap.clean_region(off, len);
    }
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(0x2400000, off); // Chunk aligned
        EXPECT_EQ(32 * Ki, len);   // Merged dirty
        bitmap.clean_region(off, len);
    }
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(ublkpp::Gi - (32 * Ki), off);
        EXPECT_EQ(32 * Ki, len); // Split dirty
        bitmap.clean_region(off, len);
    }
    EXPECT_EQ(2, bitmap.dirty_pages());
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(ublkpp::Gi, off);
        EXPECT_EQ(32 * Ki, len);
        bitmap.clean_region(off, len);
    }
    EXPECT_EQ(1, bitmap.dirty_pages());
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(5 * ublkpp::Gi, off);
        EXPECT_EQ(32 * Ki, len);
        bitmap.clean_region(off, len);
    }
    EXPECT_EQ(0, bitmap.dirty_pages());
    {
        auto [off, len] = bitmap.next_dirty();
        EXPECT_EQ(0U, len);
    }
}
