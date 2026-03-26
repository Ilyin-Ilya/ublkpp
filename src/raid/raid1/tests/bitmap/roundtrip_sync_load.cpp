#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <map>

#include "tests/test_disk.hpp"
#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/raid1_superblock.hpp"
#include "raid/raid1/tests/test_raid1_common.hpp"

using ::testing::_;

// Magic bytes for RAID1 superblock
static const uint8_t magic_bytes[16] = {0x53, 0x25, 0xff, 0x0a, 0x34, 0x99, 0x3e, 0xc5,
                                        0x67, 0x3a, 0xc8, 0x17, 0x49, 0xae, 0x1b, 0x64};

// Helper to initialize a SuperBlock with proper magic, UUID, and fields
static void init_superblock(ublkpp::raid1::SuperBlock* sb, const boost::uuids::uuid& uuid, uint32_t chunk_size = 32 * ublkpp::Ki) {
    std::memset(sb, 0, sizeof(ublkpp::raid1::SuperBlock));
    std::memcpy(sb->header.magic, magic_bytes, sizeof(magic_bytes));
    sb->header.version = htobe16(1);
    std::memcpy(sb->header.uuid, uuid.data, sizeof(sb->header.uuid));
    sb->fields.clean_unmount = 1;
    sb->fields.read_route = static_cast<uint8_t>(ublkpp::raid1::read_route::EITHER);
    sb->fields.bitmap.chunk_size = htobe32(chunk_size);
    sb->fields.bitmap.age = 0;
}

// Test full round-trip: dirty -> sync_to -> write_superblock -> load_superblock -> load_from -> verify
TEST(Raid1BitmapRoundtrip, BasicSyncAndLoad) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});

    // Storage for written data (both SuperBlock and bitmap pages)
    std::map< off_t, std::shared_ptr< uint8_t[] > > written_data;

    // Mock for all I/O operations
    EXPECT_CALL(*device, sync_iov(_, _, _, _))
        .WillRepeatedly([&written_data](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            if (op == UBLK_IO_OP_WRITE) {
                for (uint32_t i = 0; i < nr_vecs; ++i) {
                    auto page_data = std::shared_ptr< uint8_t[] >(new uint8_t[iovecs[i].iov_len]);
                    std::memcpy(page_data.get(), iovecs[i].iov_base, iovecs[i].iov_len);
                    written_data[addr + (i * iovecs[i].iov_len)] = page_data;
                }
                return nr_vecs * iovecs[0].iov_len;
            } else {
                EXPECT_EQ(1U, nr_vecs);
                auto it = written_data.find(addr);
                if (it != written_data.end()) {
                    std::memcpy(iovecs->iov_base, it->second.get(), iovecs->iov_len);
                } else {
                    std::memset(iovecs->iov_base, 0, iovecs->iov_len);
                }
                return iovecs->iov_len;
            }
        });

    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    auto uuid = boost::uuids::string_generator()(test_uuid);

    // Phase 1: Create SuperBlock and bitmap, dirty some regions
    auto sb1 = std::make_shared< ublkpp::raid1::SuperBlock >();
    init_superblock(sb1.get(), uuid);
    auto bitmap1 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb1->superbitmap_reserved);

    bitmap1.dirty_region(0, page_width);              // Page 0
    bitmap1.dirty_region(2 * page_width, page_width); // Page 2

    // Phase 2: sync_to bitmap pages and write SuperBlock
    auto sync_res = bitmap1.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(sync_res);

    auto write_sb_res = ublkpp::raid1::write_superblock(*device, sb1.get(), false, static_cast<ublkpp::raid1::read_route>(sb1->fields.read_route));
    EXPECT_TRUE(write_sb_res);

    // Phase 3: Load SuperBlock and then load bitmap from disk
    auto load_sb_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
    EXPECT_TRUE(load_sb_res);
    auto sb2 = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_sb_res.value().first, [](void* p) { free(p); });

    auto bitmap2 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb2->superbitmap_reserved);
    bitmap2.load_from(*device);

    // Phase 4: Verify loaded bitmap matches original
    EXPECT_TRUE(bitmap2.is_dirty(0, page_width));              // Page 0 should be dirty
    EXPECT_FALSE(bitmap2.is_dirty(1 * page_width, page_width)); // Page 1 should be clean
    EXPECT_TRUE(bitmap2.is_dirty(2 * page_width, page_width)); // Page 2 should be dirty

    EXPECT_EQ(2UL, bitmap2.dirty_pages());
}

// Test round-trip with many pages and batching
TEST(Raid1BitmapRoundtrip, LargeBitmapWithBatching) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 32 * ublkpp::Gi});

    std::map< off_t, std::shared_ptr< uint8_t[] > > written_data;
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    auto uuid = boost::uuids::string_generator()(test_uuid);

    // Mock for all I/O operations
    EXPECT_CALL(*device, sync_iov(_, _, _, _))
        .WillRepeatedly([&written_data](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            if (op == UBLK_IO_OP_WRITE) {
                for (uint32_t i = 0; i < nr_vecs; ++i) {
                    auto page_data = std::shared_ptr< uint8_t[] >(new uint8_t[iovecs[i].iov_len]);
                    std::memcpy(page_data.get(), iovecs[i].iov_base, iovecs[i].iov_len);
                    written_data[addr + (i * iovecs[i].iov_len)] = page_data;
                }
                return nr_vecs * iovecs[0].iov_len;
            } else {
                auto it = written_data.find(addr);
                if (it != written_data.end()) {
                    std::memcpy(iovecs->iov_base, it->second.get(), iovecs->iov_len);
                } else {
                    std::memset(iovecs->iov_base, 0, iovecs->iov_len);
                }
                return iovecs->iov_len;
            }
        });

    // Phase 1: Create SuperBlock and bitmap, dirty many consecutive pages
    auto sb1 = std::make_shared< ublkpp::raid1::SuperBlock >();
    init_superblock(sb1.get(), uuid);
    auto bitmap1 = ublkpp::raid1::Bitmap(32 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb1->superbitmap_reserved);

    bitmap1.dirty_region(0, 10 * page_width); // Dirty pages 0-9 (10 consecutive pages)

    // Phase 2: sync_to with batching and write SuperBlock
    auto sync_res = bitmap1.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(sync_res);

    auto write_sb_res = ublkpp::raid1::write_superblock(*device, sb1.get(), false, static_cast<ublkpp::raid1::read_route>(sb1->fields.read_route));
    EXPECT_TRUE(write_sb_res);

    // Phase 3: Load SuperBlock and bitmap
    auto load_sb_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
    EXPECT_TRUE(load_sb_res);
    auto sb2 = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_sb_res.value().first, [](void* p) { free(p); });

    auto bitmap2 = ublkpp::raid1::Bitmap(32 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb2->superbitmap_reserved);
    bitmap2.load_from(*device);

    // Phase 4: Verify all 10 pages are dirty
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(bitmap2.is_dirty(i * page_width, page_width))
            << "Page " << i << " should be dirty";
    }
    EXPECT_FALSE(bitmap2.is_dirty(10 * page_width, page_width)); // Page 10 should be clean

    EXPECT_EQ(10UL, bitmap2.dirty_pages());
}

// Test round-trip with modifications after load
TEST(Raid1BitmapRoundtrip, ModifyAfterLoad) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});

    std::map< off_t, std::shared_ptr< uint8_t[] > > storage;
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    auto uuid = boost::uuids::string_generator()(test_uuid);

    // Setup mock for all reads/writes
    EXPECT_CALL(*device, sync_iov(_, _, _, _))
        .WillRepeatedly([&storage](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            if (op == UBLK_IO_OP_WRITE) {
                for (uint32_t i = 0; i < nr_vecs; ++i) {
                    auto page_data = std::shared_ptr< uint8_t[] >(new uint8_t[iovecs[i].iov_len]);
                    std::memcpy(page_data.get(), iovecs[i].iov_base, iovecs[i].iov_len);
                    storage[addr + (i * iovecs[i].iov_len)] = page_data;
                }
                return nr_vecs * iovecs[0].iov_len;
            } else {
                auto it = storage.find(addr);
                if (it != storage.end()) {
                    std::memcpy(iovecs->iov_base, it->second.get(), iovecs->iov_len);
                } else {
                    std::memset(iovecs->iov_base, 0, iovecs->iov_len);
                }
                return iovecs->iov_len;
            }
        });

    // Round 1: Create SuperBlock and bitmap, dirty page 0, sync
    {
        auto sb = std::make_shared< ublkpp::raid1::SuperBlock >();
        init_superblock(sb.get(), uuid);
        auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb->superbitmap_reserved);
        bitmap.dirty_region(0, page_width);
        bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
        ublkpp::raid1::write_superblock(*device, sb.get(), false, static_cast<ublkpp::raid1::read_route>(sb->fields.read_route));
    }

    // Round 2: Load SuperBlock, load bitmap, modify, sync
    {
        auto load_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
        EXPECT_TRUE(load_res);
        auto sb = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_res.value().first, [](void* p) { free(p); });

        auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb->superbitmap_reserved);
        bitmap.load_from(*device);

        EXPECT_TRUE(bitmap.is_dirty(0, page_width)); // Verify page 0 from round 1

        bitmap.dirty_region(1 * page_width, page_width); // Dirty page 1
        bitmap.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
        ublkpp::raid1::write_superblock(*device, sb.get(), false, static_cast<ublkpp::raid1::read_route>(sb->fields.read_route));
    }

    // Round 3: Load and verify both pages are dirty
    {
        auto load_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
        EXPECT_TRUE(load_res);
        auto sb = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_res.value().first, [](void* p) { free(p); });

        auto bitmap = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb->superbitmap_reserved);
        bitmap.load_from(*device);

        EXPECT_TRUE(bitmap.is_dirty(0, page_width)); // Page 0 (from round 1)
        EXPECT_TRUE(bitmap.is_dirty(1 * page_width, page_width)); // Page 1 (from round 2)
        EXPECT_EQ(2UL, bitmap.dirty_pages());
    }
}

// Test that cleaned regions don't persist
TEST(Raid1BitmapRoundtrip, CleanedRegionsDontPersist) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});

    std::map< off_t, std::shared_ptr< uint8_t[] > > storage;
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    auto uuid = boost::uuids::string_generator()(test_uuid);

    // Mock for all I/O operations
    EXPECT_CALL(*device, sync_iov(_, _, _, _))
        .WillRepeatedly([&storage](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            if (op == UBLK_IO_OP_WRITE) {
                for (uint32_t i = 0; i < nr_vecs; ++i) {
                    auto page_data = std::shared_ptr< uint8_t[] >(new uint8_t[iovecs[i].iov_len]);
                    std::memcpy(page_data.get(), iovecs[i].iov_base, iovecs[i].iov_len);
                    storage[addr + (i * iovecs[i].iov_len)] = page_data;
                }
                return nr_vecs * iovecs[0].iov_len;
            } else {
                auto it = storage.find(addr);
                if (it != storage.end()) {
                    std::memcpy(iovecs->iov_base, it->second.get(), iovecs->iov_len);
                } else {
                    std::memset(iovecs->iov_base, 0, iovecs->iov_len);
                }
                return iovecs->iov_len;
            }
        });

    // Phase 1: Create SuperBlock, dirty then clean
    auto sb1 = std::make_shared< ublkpp::raid1::SuperBlock >();
    init_superblock(sb1.get(), uuid);
    auto bitmap1 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb1->superbitmap_reserved);

    bitmap1.dirty_region(0, page_width);
    bitmap1.clean_region(0, page_width); // Clean it immediately

    // Phase 2: sync_to should not write bitmap pages (only SuperBlock)
    auto sync_res = bitmap1.sync_to(*device, ublkpp::raid1::Bitmap::page_size());
    EXPECT_TRUE(sync_res);

    auto write_sb_res = ublkpp::raid1::write_superblock(*device, sb1.get(), false, static_cast<ublkpp::raid1::read_route>(sb1->fields.read_route));
    EXPECT_TRUE(write_sb_res);

    // Phase 3: Load should find no dirty pages
    auto load_sb_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
    EXPECT_TRUE(load_sb_res);
    auto sb2 = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_sb_res.value().first, [](void* p) { free(p); });

    auto bitmap2 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb2->superbitmap_reserved);
    bitmap2.load_from(*device);

    EXPECT_FALSE(bitmap2.is_dirty(0, page_width));
    EXPECT_EQ(0UL, bitmap2.dirty_pages());
}

// Test that bitmap pages are written/read at correct offset (after SuperBlock)
TEST(Raid1BitmapRoundtrip, WithSuperBlockOffset) {
    auto device = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = 8 * ublkpp::Gi});

    std::map< off_t, std::shared_ptr< uint8_t[] > > storage;
    auto page_width = 32 * ublkpp::Ki * 4 * ublkpp::Ki * 8UL;
    constexpr auto OFFSET = 4096UL; // sizeof(SuperBlock)
    auto uuid = boost::uuids::string_generator()(test_uuid);

    // Mock for all I/O operations
    EXPECT_CALL(*device, sync_iov(_, _, _, _))
        .WillRepeatedly([&storage, OFFSET](uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) -> ublkpp::io_result {
            if (op == UBLK_IO_OP_WRITE) {
                // Verify bitmap pages are written after SuperBlock
                if (addr > 0) {
                    EXPECT_GE(addr, OFFSET) << "Bitmap pages should be written after SuperBlock at offset " << OFFSET;
                }
                for (uint32_t i = 0; i < nr_vecs; ++i) {
                    auto page_data = std::shared_ptr< uint8_t[] >(new uint8_t[iovecs[i].iov_len]);
                    std::memcpy(page_data.get(), iovecs[i].iov_base, iovecs[i].iov_len);
                    storage[addr + (i * iovecs[i].iov_len)] = page_data;
                }
                return nr_vecs * iovecs[0].iov_len;
            } else {
                auto it = storage.find(addr);
                if (it != storage.end()) {
                    std::memcpy(iovecs->iov_base, it->second.get(), iovecs->iov_len);
                } else {
                    std::memset(iovecs->iov_base, 0, iovecs->iov_len);
                }
                return iovecs->iov_len;
            }
        });

    // Phase 1: Create SuperBlock and bitmap, dirty page 0
    auto sb1 = std::make_shared< ublkpp::raid1::SuperBlock >();
    init_superblock(sb1.get(), uuid);
    auto bitmap1 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb1->superbitmap_reserved);
    bitmap1.dirty_region(0, page_width);

    bitmap1.sync_to(*device, OFFSET);
    ublkpp::raid1::write_superblock(*device, sb1.get(), false, static_cast<ublkpp::raid1::read_route>(sb1->fields.read_route));

    // Phase 2: Load SuperBlock and bitmap
    auto load_res = ublkpp::raid1::load_superblock(*device, uuid, 32 * ublkpp::Ki);
    EXPECT_TRUE(load_res);
    auto sb2 = std::shared_ptr< ublkpp::raid1::SuperBlock >(load_res.value().first, [](void* p) { free(p); });

    auto bitmap2 = ublkpp::raid1::Bitmap(8 * ublkpp::Gi, 32 * ublkpp::Ki, 4 * ublkpp::Ki, sb2->superbitmap_reserved);
    bitmap2.load_from(*device);

    // Verify page loaded correctly
    EXPECT_TRUE(bitmap2.is_dirty(0, page_width));
    EXPECT_EQ(1UL, bitmap2.dirty_pages());
}
