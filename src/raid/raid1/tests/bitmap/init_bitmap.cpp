#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <isa-l/mem_routines.h>

#include "tests/test_disk.hpp"
#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/raid1_superblock.hpp"
#include "raid/raid1/tests/test_raid1_common.hpp"

using ::testing::_;
#ifndef Ki
using ublkpp::Gi;
using ublkpp::Ki;
#endif

// Ensure that all required pages are initialized
TEST(Raid1, InitBitmap) {
    auto const capacity = 100 * Gi;
    auto const chunk_size = 32 * Ki;

    auto const device_params = TestParams{.capacity = capacity};
    auto const block_size = device_params.l_size;
    auto device = std::make_shared< ublkpp::TestDisk >(device_params);
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(capacity, chunk_size, block_size, superbitmap_buf.get());

    // Each page is 4k and represents 32KB of data per bit. Calculations follow for how bitmap clearning should occur.
    auto const max_pages = device->max_tx() / ublkpp::raid1::Bitmap::page_size();
    auto const page_width = (chunk_size * ublkpp::raid1::Bitmap::page_size() * ublkpp::raid1::k_bits_in_byte);
    auto const num_pages = capacity / page_width + ((0 == capacity % page_width) ? 0 : 1);
    auto const num_writes = num_pages / max_pages + ((0 == num_pages % max_pages) ? 0 : 1);

    auto total_written = 0U;
    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(num_writes)
        .WillRepeatedly([max_tx = device->max_tx(), &total_written,
                         &max_pages](uint8_t, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            EXPECT_LE(nr_vecs, max_pages);
            total_written += ublkpp::__iovec_len(iovecs, iovecs + nr_vecs);
            EXPECT_LE(ublkpp::__iovec_len(iovecs, iovecs + nr_vecs), max_tx);
            EXPECT_GE(addr, ublkpp::raid1::Bitmap::page_size());       // Expect write to bitmap!
            EXPECT_LE(addr, 100 * ublkpp::raid1::Bitmap::page_size()); // Expect write to bitmap!
            EXPECT_EQ(0, isal_zero_detect(iovecs->iov_base, ublkpp::raid1::Bitmap::page_size()));
            return ublkpp::raid1::Bitmap::page_size();
        });
    bitmap.init_to(*device);
    EXPECT_EQ(num_pages * ublkpp::raid1::Bitmap::page_size(), total_written);
}

// Ensure that all required pages are initialized
TEST(Raid1, InitBitmapFailure) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 2 * Gi});
    auto superbitmap_buf = make_test_superbitmap();
    auto bitmap = ublkpp::raid1::Bitmap(2 * Gi, 32 * Ki, 4 * Ki, superbitmap_buf.get());

    EXPECT_CALL(*device, sync_iov(UBLK_IO_OP_WRITE, _, _, _))
        .Times(1)
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> ublkpp::io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    EXPECT_THROW(bitmap.init_to(*device), std::runtime_error);
}
