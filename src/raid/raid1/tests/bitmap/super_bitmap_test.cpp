#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "raid/raid1/super_bitmap.hpp"
#include "raid/raid1/raid1_superblock.hpp"

using namespace ublkpp::raid1;

// Test fixture for SuperBitmap tests
class SuperBitmapTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate and zero-initialize buffer
        buffer = std::make_unique<uint8_t[]>(k_superbitmap_size);
        std::memset(buffer.get(), 0, k_superbitmap_size);
    }

    std::unique_ptr<uint8_t[]> buffer;
};

// Basic functionality tests
TEST_F(SuperBitmapTest, SetAndTestBit) {
    SuperBitmap sb(buffer.get());

    // Initially all bits should be clear
    EXPECT_FALSE(sb.test_bit(0));
    EXPECT_FALSE(sb.test_bit(100));
    EXPECT_FALSE(sb.test_bit(32175));

    // Set some bits
    sb.set_bit(0);
    sb.set_bit(100);
    sb.set_bit(32175);

    // Verify they are set
    EXPECT_TRUE(sb.test_bit(0));
    EXPECT_TRUE(sb.test_bit(100));
    EXPECT_TRUE(sb.test_bit(32175));

    // Verify other bits remain clear
    EXPECT_FALSE(sb.test_bit(1));
    EXPECT_FALSE(sb.test_bit(99));
    EXPECT_FALSE(sb.test_bit(32174));
}

TEST_F(SuperBitmapTest, ClearBit) {
    SuperBitmap sb(buffer.get());

    // Set a bit
    sb.set_bit(42);
    EXPECT_TRUE(sb.test_bit(42));

    // Clear it
    sb.clear_bit(42);
    EXPECT_FALSE(sb.test_bit(42));

    // Clear an already-clear bit (should be no-op)
    sb.clear_bit(43);
    EXPECT_FALSE(sb.test_bit(43));
}

TEST_F(SuperBitmapTest, MultipleBitsInSameByte) {
    SuperBitmap sb(buffer.get());

    // Bits 0-7 are in the same byte
    sb.set_bit(0);
    sb.set_bit(3);
    sb.set_bit(7);

    EXPECT_TRUE(sb.test_bit(0));
    EXPECT_FALSE(sb.test_bit(1));
    EXPECT_FALSE(sb.test_bit(2));
    EXPECT_TRUE(sb.test_bit(3));
    EXPECT_FALSE(sb.test_bit(4));
    EXPECT_FALSE(sb.test_bit(5));
    EXPECT_FALSE(sb.test_bit(6));
    EXPECT_TRUE(sb.test_bit(7));

    // Clear one bit
    sb.clear_bit(3);
    EXPECT_FALSE(sb.test_bit(3));
    EXPECT_TRUE(sb.test_bit(0));  // Others unchanged
    EXPECT_TRUE(sb.test_bit(7));
}

TEST_F(SuperBitmapTest, BoundaryValues) {
    SuperBitmap sb(buffer.get());

    // Test first bit
    sb.set_bit(0);
    EXPECT_TRUE(sb.test_bit(0));

    // Test last valid bit (32,176 - 1 = 32,175)
    sb.set_bit(32175);
    EXPECT_TRUE(sb.test_bit(32175));

    // Test last bit in first byte
    sb.set_bit(7);
    EXPECT_TRUE(sb.test_bit(7));

    // Test first bit in last byte
    sb.set_bit(32168);  // 4021 * 8 = 32,168
    EXPECT_TRUE(sb.test_bit(32168));
}

TEST_F(SuperBitmapTest, ClearAll) {
    SuperBitmap sb(buffer.get());

    // Set some bits
    sb.set_bit(0);
    sb.set_bit(100);
    sb.set_bit(1000);
    sb.set_bit(32175);

    EXPECT_TRUE(sb.test_bit(0));
    EXPECT_TRUE(sb.test_bit(100));
    EXPECT_TRUE(sb.test_bit(1000));
    EXPECT_TRUE(sb.test_bit(32175));

    // Clear all
    sb.clear_all();

    // Verify all are clear
    EXPECT_FALSE(sb.test_bit(0));
    EXPECT_FALSE(sb.test_bit(100));
    EXPECT_FALSE(sb.test_bit(1000));
    EXPECT_FALSE(sb.test_bit(32175));
}

TEST_F(SuperBitmapTest, DataPointer) {
    SuperBitmap sb(buffer.get());

    // Verify data() returns the original buffer
    EXPECT_EQ(sb.data(), buffer.get());

    // Modify via data pointer
    sb.data()[0] = 0xFF;

    // Verify all 8 bits in first byte are set
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(sb.test_bit(i));
    }
}

TEST_F(SuperBitmapTest, PreservesExistingData) {
    // Pre-populate buffer with some pattern
    buffer[0] = 0b10101010;  // Bits 0,2,4,6 set
    buffer[100] = 0b11110000; // Bits 800-803 set

    SuperBitmap sb(buffer.get());

    // Verify pre-existing bits are preserved (constructor doesn't clear)
    EXPECT_FALSE(sb.test_bit(0));
    EXPECT_TRUE(sb.test_bit(1));
    EXPECT_FALSE(sb.test_bit(2));
    EXPECT_TRUE(sb.test_bit(3));

    EXPECT_TRUE(sb.test_bit(804));
    EXPECT_TRUE(sb.test_bit(805));
    EXPECT_TRUE(sb.test_bit(806));
    EXPECT_TRUE(sb.test_bit(807));
}

// Concurrency tests
TEST_F(SuperBitmapTest, ConcurrentSetBitDifferentBytes) {
    SuperBitmap sb(buffer.get());

    constexpr int num_threads = 10;
    constexpr int bits_per_thread = 100;
    std::vector<std::thread> threads;

    // Each thread sets bits in a different range
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&sb, t]() {
            int start_bit = t * 1000;
            for (int i = 0; i < bits_per_thread; ++i) {
                sb.set_bit(start_bit + i);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all bits were set correctly
    for (int t = 0; t < num_threads; ++t) {
        int start_bit = t * 1000;
        for (int i = 0; i < bits_per_thread; ++i) {
            EXPECT_TRUE(sb.test_bit(start_bit + i))
                << "Bit " << (start_bit + i) << " should be set";
        }
    }
}

TEST_F(SuperBitmapTest, ConcurrentSetBitSameByte) {
    SuperBitmap sb(buffer.get());

    constexpr int num_threads = 8;  // One thread per bit in first byte
    std::vector<std::thread> threads;

    // Each thread sets a different bit in the same byte
    for (int bit = 0; bit < num_threads; ++bit) {
        threads.emplace_back([&sb, bit]() {
            // Set the same bit multiple times to stress-test atomicity
            for (int i = 0; i < 1000; ++i) {
                sb.set_bit(bit);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all 8 bits in first byte are set
    for (int bit = 0; bit < num_threads; ++bit) {
        EXPECT_TRUE(sb.test_bit(bit))
            << "Bit " << bit << " should be set";
    }
}

TEST_F(SuperBitmapTest, ConcurrentSetAndClearDifferentBits) {
    SuperBitmap sb(buffer.get());

    // Pre-set some bits
    for (int i = 0; i < 16; ++i) {
        if (i % 2 == 0) {
            sb.set_bit(i);
        }
    }

    std::vector<std::thread> threads;

    // Half the threads set odd bits, half clear even bits
    for (int bit = 0; bit < 16; ++bit) {
        threads.emplace_back([&sb, bit]() {
            for (int i = 0; i < 500; ++i) {
                if (bit % 2 == 1) {
                    sb.set_bit(bit);  // Set odd bits
                } else {
                    sb.clear_bit(bit);  // Clear even bits
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify final state: odd bits set, even bits clear
    for (int bit = 0; bit < 16; ++bit) {
        if (bit % 2 == 1) {
            EXPECT_TRUE(sb.test_bit(bit))
                << "Odd bit " << bit << " should be set";
        } else {
            EXPECT_FALSE(sb.test_bit(bit))
                << "Even bit " << bit << " should be clear";
        }
    }
}

TEST_F(SuperBitmapTest, ConcurrentReadWhileWrite) {
    SuperBitmap sb(buffer.get());

    std::atomic<bool> stop{false};
    std::atomic<int> read_count{0};

    // Writer thread: sets bits 0-99
    std::thread writer([&]() {
        for (int i = 0; i < 1000; ++i) {
            for (int bit = 0; bit < 100; ++bit) {
                sb.set_bit(bit);
            }
        }
        stop.store(true);
    });

    // Reader threads: read bits 0-99
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stop.load()) {
                for (int bit = 0; bit < 100; ++bit) {
                    (void)sb.test_bit(bit);  // Just read, don't care about result
                    read_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    // All bits should be set after writer completes
    for (int bit = 0; bit < 100; ++bit) {
        EXPECT_TRUE(sb.test_bit(bit));
    }

    // Verify readers actually ran
    EXPECT_GT(read_count.load(), 0);
}

// Integration test with actual SuperBlock
TEST_F(SuperBitmapTest, IntegrationWithSuperBlock) {
    // Allocate a real SuperBlock
    auto sb_ptr = static_cast<SuperBlock*>(aligned_alloc(4096, sizeof(SuperBlock)));
    ASSERT_NE(sb_ptr, nullptr);
    std::memset(sb_ptr, 0, sizeof(SuperBlock));

    // Create SuperBitmap pointing to the superbitmap_reserved field
    SuperBitmap superbitmap(sb_ptr->superbitmap_reserved);

    // Set some bits
    superbitmap.set_bit(0);
    superbitmap.set_bit(100);
    superbitmap.set_bit(32175);

    // Verify the bits are actually in the SuperBlock memory
    EXPECT_EQ(sb_ptr->superbitmap_reserved[0] & 0x01, 0x01);
    EXPECT_EQ(sb_ptr->superbitmap_reserved[12] & 0x10, 0x10);  // Bit 100 = byte 12, bit 4
    EXPECT_EQ(sb_ptr->superbitmap_reserved[4021] & 0x80, 0x80); // Bit 32175 = byte 4021, bit 7

    free(sb_ptr);
}
