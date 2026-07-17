#include "test_raid1_common.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>

#include <boost/uuid/string_generator.hpp>

#include "raid/raid1/bitmap.hpp"
#include "raid/raid1/raid1_impl.hpp"
#include "raid/raid1/raid1_resync_task.hpp"

using namespace std::chrono_literals;
using namespace ublkpp::raid1;
using ::testing::_;
using ::testing::AnyNumber;
using ::ublkpp::io_result;

namespace {

constexpr uint32_t kPage = 4 * Ki;

// A sparse, page-granular backing store modeling a thinly-provisioned device: a page that was
// never written reads back as zero and is *not* allocated (has_page() == false). Records read/write
// counts and total bytes written so tests can assert exactly which pages a resync mode touched.
struct BackingStore {
    std::map< uint64_t, std::array< uint8_t, kPage > > pages;
    std::atomic< int > reads{0};
    std::atomic< int > writes{0};
    std::atomic< uint64_t > write_bytes{0};

    io_result read(iovec* iov, off_t addr) {
        reads.fetch_add(1, std::memory_order_relaxed);
        auto* buf = static_cast< uint8_t* >(iov->iov_base);
        auto const len = iov->iov_len;
        for (uint64_t off = 0; off < len; off += kPage) {
            auto const n = std::min< uint64_t >(kPage, len - off);
            if (auto it = pages.find(static_cast< uint64_t >(addr) + off); it != pages.end())
                std::memcpy(buf + off, it->second.data(), n);
            else
                std::memset(buf + off, 0, n); // thin: never-written pages read zero
        }
        return static_cast< int >(len);
    }

    io_result write(iovec* iov, off_t addr) {
        writes.fetch_add(1, std::memory_order_relaxed);
        write_bytes.fetch_add(iov->iov_len, std::memory_order_relaxed);
        auto const* buf = static_cast< uint8_t const* >(iov->iov_base);
        auto const len = iov->iov_len;
        for (uint64_t off = 0; off < len; off += kPage) {
            std::array< uint8_t, kPage > pg{};
            std::memcpy(pg.data(), buf + off, std::min< uint64_t >(kPage, len - off));
            pages[static_cast< uint64_t >(addr) + off] = pg;
        }
        return static_cast< int >(len);
    }

    void seed_page(uint64_t addr, uint8_t fill) {
        std::array< uint8_t, kPage > pg{};
        std::memset(pg.data(), fill, kPage);
        pages[addr] = pg;
    }
    bool has_page(uint64_t addr) const { return pages.contains(addr); }
    uint8_t first_byte(uint64_t addr) const {
        auto it = pages.find(addr);
        return (it == pages.end()) ? 0 : it->second[0];
    }
    void reset_counters() {
        reads.store(0, std::memory_order_relaxed);
        writes.store(0, std::memory_order_relaxed);
        write_bytes.store(0, std::memory_order_relaxed);
    }
};

void wire(ublkpp::TestDisk& disk, BackingStore& store) {
    EXPECT_CALL(disk, sync_iov(_, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([&store](uint8_t op, iovec* iov, uint32_t, off_t addr) -> io_result {
            return (op == UBLK_IO_OP_READ) ? store.read(iov, addr) : store.write(iov, addr);
        });
}

bool wait_for_bitmap_clean(std::shared_ptr< Bitmap > const& bitmap, std::chrono::milliseconds timeout = 3000ms) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (0 == bitmap->dirty_pages()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

class ResyncCopyModeTest : public ::testing::Test {
protected:
    // A logical region well past the bitmap pages so resync data I/O never overlaps the bitmap
    // page __clean() writes back to the clean leg (which land at low addresses).
    static constexpr uint64_t kBase = 512 * Mi;
    static constexpr uint32_t kOffset = kPage;    // reserved offset passed to the resync
    static constexpr uint32_t kRegion = 128 * Ki; // 32 pages copied in one __copy_region
    static constexpr uint32_t kPages = kRegion / kPage;

    BackingStore src_store, dst_store;
    std::shared_ptr< ublkpp::TestDisk > src_disk, dst_disk;
    std::shared_ptr< MirrorDevice > clean_mirror, dirty_mirror;
    std::unique_ptr< uint8_t[] > superbitmap_buf;
    std::shared_ptr< Bitmap > bitmap;

    void SetUp() override {
        src_disk = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "src"});
        dst_disk = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "dst", .is_slot_b = true});
        wire(*src_disk, src_store);
        wire(*dst_disk, dst_store);

        auto uuid = boost::uuids::string_generator()(test_uuid);
        clean_mirror = std::make_shared< MirrorDevice >(uuid, src_disk);
        dirty_mirror = std::make_shared< MirrorDevice >(uuid, dst_disk);

        superbitmap_buf = make_test_superbitmap();
        bitmap = std::make_shared< Bitmap >(Gi, 32 * Ki, kPage, superbitmap_buf.get());
        bitmap->dirty_region(kBase, kRegion);

        // Discard superblock-load I/O done during MirrorDevice construction.
        src_store.reset_counters();
        dst_store.reset_counters();
    }

    uint64_t page_addr(uint32_t p) const { return kBase + kOffset + static_cast< uint64_t >(p) * kPage; }
    void seed_src(uint32_t p, uint8_t fill) { src_store.seed_page(page_addr(p), fill); }
    void seed_dst(uint32_t p, uint8_t fill) { dst_store.seed_page(page_addr(p), fill); }

    void run(resync_copy_mode mode) {
        // live outlives task (declared first) so the task's join completes before it is destroyed.
        std::atomic< resync_copy_mode > live{mode};
        Raid1ResyncTask task{bitmap, kOffset, kPage, 256u * Ki, &live};
        task.launch(test_uuid, clean_mirror, dirty_mirror, [] { return true; });
        ASSERT_TRUE(wait_for_bitmap_clean(bitmap)) << "resync did not converge";
        task.stop();
    }
};

// CHECK: identical legs except a contiguous divergent pair (10,11) and a lone divergent page (20).
// Only the divergent pages are written (coalesced into two writes); identical pages are skipped;
// the destination is read for comparison; end state matches the source.
TEST_F(ResyncCopyModeTest, CheckWritesOnlyDivergentPages) {
    for (uint32_t p = 0; p < kPages; ++p) {
        seed_src(p, 0x11);
        seed_dst(p, 0x11);
    }
    seed_src(10, 0x22);
    seed_src(11, 0x22);
    seed_src(20, 0x33);
    src_store.reset_counters();
    dst_store.reset_counters();

    run(resync_copy_mode::CHECK);

    EXPECT_GT(dst_store.reads.load(), 0) << "CHECK must read the destination to compare";
    EXPECT_EQ(3u * kPage, dst_store.write_bytes.load()) << "only the 3 divergent pages should be written";
    EXPECT_EQ(2, dst_store.writes.load()) << "{10,11} coalesce into one write; {20} is a second";
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(10)));
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(11)));
    EXPECT_EQ(0x33, dst_store.first_byte(page_addr(20)));
    EXPECT_EQ(0x11, dst_store.first_byte(page_addr(5))) << "identical page must be left untouched";
}

// ZERO_TEST: source has a contiguous non-zero run (10,11,12), zero elsewhere; destination starts
// empty. The destination is never read; only the non-zero pages are written (one coalesced write);
// zero pages are neither written nor allocated (thin preserved).
TEST_F(ResyncCopyModeTest, ZeroTestSkipsZeroPagesAndNeverReadsDest) {
    seed_src(10, 0x22);
    seed_src(11, 0x22);
    seed_src(12, 0x22);
    src_store.reset_counters();
    dst_store.reset_counters();

    run(resync_copy_mode::ZERO_TEST);

    EXPECT_EQ(0, dst_store.reads.load()) << "ZERO_TEST must not read the destination";
    EXPECT_EQ(3u * kPage, dst_store.write_bytes.load()) << "only the 3 non-zero pages should be written";
    EXPECT_EQ(1, dst_store.writes.load()) << "the contiguous non-zero run coalesces into one write";
    EXPECT_TRUE(dst_store.has_page(page_addr(10)));
    EXPECT_TRUE(dst_store.has_page(page_addr(12)));
    EXPECT_FALSE(dst_store.has_page(page_addr(5))) << "zero page must stay unallocated (thin)";
    EXPECT_FALSE(dst_store.has_page(page_addr(31)));
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(10)));
}

// BLIND: writes the entire region in one shot, provisioning every page (including zero ones), and
// never reads the destination -- the contrast to ZERO_TEST's thin behavior.
TEST_F(ResyncCopyModeTest, BlindWritesWholeRegionProvisioningZeros) {
    seed_src(10, 0x22);
    seed_src(11, 0x22);
    src_store.reset_counters();
    dst_store.reset_counters();

    run(resync_copy_mode::BLIND);

    EXPECT_EQ(0, dst_store.reads.load()) << "BLIND does not read the destination";
    EXPECT_EQ(kRegion, dst_store.write_bytes.load()) << "the whole region is written";
    EXPECT_EQ(1, dst_store.writes.load());
    EXPECT_TRUE(dst_store.has_page(page_addr(5))) << "BLIND provisions even zero pages";
    EXPECT_TRUE(dst_store.has_page(page_addr(31)));
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(10)));
    EXPECT_EQ(0x00, dst_store.first_byte(page_addr(5)));
}

// The compare-equal-skip path must still pass through the region_tracker post-check: a write held
// in-flight over the region keeps it dirty even though the legs are identical (CHECK would skip),
// and once the write completes the region cleans with no destination write at all.
TEST_F(ResyncCopyModeTest, CheckSkipStillHonorsInflightWrite) {
    for (uint32_t p = 0; p < kPages; ++p) {
        seed_src(p, 0x11);
        seed_dst(p, 0x11);
    }
    src_store.reset_counters();
    dst_store.reset_counters();

    std::atomic< resync_copy_mode > live{resync_copy_mode::CHECK};
    Raid1ResyncTask task{bitmap, kOffset, kPage, 256u * Ki, &live};
    task.enqueue_write(kBase, kRegion); // conflicting write held in-flight
    task.launch(test_uuid, clean_mirror, dirty_mirror, [] { return true; });

    // Let the resync sweep at least once; the conflicting range must not be cleaned.
    auto const deadline = std::chrono::steady_clock::now() + 2000ms;
    while (task.yield_count() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_GT(bitmap->dirty_pages(), 0u) << "region must stay dirty while the write is in-flight";

    task.dequeue_write(kBase, kRegion);
    EXPECT_TRUE(wait_for_bitmap_clean(bitmap)) << "region must clean once the write completes";
    EXPECT_EQ(0, dst_store.writes.load()) << "identical legs: compare-skip cleans without any write";
    task.stop();
}

// CHECK converges the destination to the source for every kind of page, including the hazard case
// a genuinely-populated leg presents: source zero but destination non-zero. CHECK reads the dest,
// sees the difference, and writes the zero -- healing the divergence.
TEST_F(ResyncCopyModeTest, CheckHealsZeroSourceDivergence) {
    for (uint32_t p = 0; p < kPages; ++p) {
        seed_src(p, 0x11);
        seed_dst(p, 0x11);
    }
    seed_src(3, 0x22); // source non-zero, dest 0x11 -> differ
    seed_src(7, 0x00); // source ZERO, dest 0x11 -> differ (the hazard ZERO_TEST would skip)
    src_store.reset_counters();
    dst_store.reset_counters();

    run(resync_copy_mode::CHECK);

    for (uint32_t p = 0; p < kPages; ++p)
        EXPECT_EQ(src_store.first_byte(page_addr(p)), dst_store.first_byte(page_addr(p)))
            << "page " << p << " did not converge to the source";
    EXPECT_EQ(0x00, dst_store.first_byte(page_addr(7))) << "zero-source divergence must be healed";
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(3)));
}

// Documents WHY ZERO_TEST is gated on genuine freshness: on a leg that already holds real data,
// a zero source page is skipped and the destination keeps its stale non-zero data -- silent
// divergence. This is exactly the misuse the sb_was_fresh gate prevents (only a genuinely-fresh leg,
// which reads zero where unallocated, may use ZERO_TEST).
TEST_F(ResyncCopyModeTest, ZeroTestLeavesZeroSourceDivergenceOnPopulatedDest) {
    seed_dst(7, 0x11);  // destination already holds real data at page 7
    seed_src(7, 0x00);  // source is zero there
    seed_src(10, 0x22); // a genuine non-zero page (gets written)
    src_store.reset_counters();
    dst_store.reset_counters();

    run(resync_copy_mode::ZERO_TEST);

    EXPECT_EQ(0x11, dst_store.first_byte(page_addr(7)))
        << "ZERO_TEST skips the zero source and leaves the populated dest stale (why the gate exists)";
    EXPECT_EQ(0x22, dst_store.first_byte(page_addr(10)));
}

// A destination compare-read failure in CHECK is NOT fatal: the run falls back to a BLIND write of
// the source (the write may remap a media error to a fresh LBA), healing the region instead of
// looping on the failing read. Here the dest read fails but writes succeed, so the divergent legs
// converge and the region cleans.
TEST_F(ResyncCopyModeTest, CheckDestReadFailureFallsBackToBlindWrite) {
    for (uint32_t p = 0; p < kPages; ++p) {
        seed_src(p, 0x11);
        seed_dst(p, 0x22); // legs differ; the fallback BLIND write must converge them
    }
    src_store.reset_counters();
    dst_store.reset_counters();
    // Fail the destination compare-read (added after SetUp's generic wiring, so it wins for READs);
    // writes still succeed via the generic store.write.
    EXPECT_CALL(*dst_disk, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });

    run(resync_copy_mode::CHECK);

    EXPECT_GT(dst_store.writes.load(), 0) << "CHECK dest-read failure must fall back to a BLIND write";
    EXPECT_EQ(0x11, dst_store.first_byte(page_addr(0))) << "the source data is written to the destination";
    EXPECT_EQ(0x11, dst_store.first_byte(page_addr(kPages - 1))) << "the whole region is written";
}

// sb_was_fresh (the ZERO_TEST eligibility gate) reflects genuine freshness -- SB magic absent --
// not the age-promoted new_device flag. A valid on-disk superblock => not fresh; a zeroed device
// (no magic) => fresh.
TEST(Raid1MirrorDevice, SbWasFreshReflectsGenuineFreshness) {
    auto uuid = boost::uuids::string_generator()(test_uuid);

    auto valid_disk = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "valid"});
    EXPECT_CALL(*valid_disk, sync_iov(_, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([](uint8_t op, iovec* iov, uint32_t, off_t) -> io_result {
            if (op == UBLK_IO_OP_READ && iov->iov_base) std::memcpy(iov->iov_base, &normal_superblock, k_page_size);
            return static_cast< int >(iov->iov_len);
        });
    MirrorDevice valid_mirror(uuid, valid_disk);
    EXPECT_FALSE(valid_mirror.sb_was_fresh) << "a leg with a valid superblock is not genuinely fresh";
    EXPECT_FALSE(valid_mirror.new_device);

    auto fresh_disk = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "fresh"});
    EXPECT_CALL(*fresh_disk, sync_iov(_, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([](uint8_t op, iovec* iov, uint32_t, off_t) -> io_result {
            if (op == UBLK_IO_OP_READ && iov->iov_base) std::memset(iov->iov_base, 0, iov->iov_len);
            return static_cast< int >(iov->iov_len);
        });
    MirrorDevice fresh_mirror(uuid, fresh_disk);
    EXPECT_TRUE(fresh_mirror.sb_was_fresh) << "a zeroed device (no superblock magic) is genuinely fresh";
    EXPECT_TRUE(fresh_mirror.new_device);
}

// Option B: a write that races a ZERO_TEST copy downgrades the run to CHECK, so the deferred
// region is re-synced by reading and comparing the destination rather than trusting a stale
// zero-skip. The write is injected synchronously during the first source read (between the Phase-1
// and Phase-2 conflict checks), which forces Phase-2 to defer and downgrade. With identical legs a
// pure ZERO_TEST run would never read the destination, so any destination read proves CHECK ran.
TEST_F(ResyncCopyModeTest, ZeroTestDowngradesToCheckOnWriteConflict) {
    for (uint32_t p = 0; p < kPages; ++p) {
        seed_src(p, 0x11);
        seed_dst(p, 0x11);
    }
    src_store.reset_counters();
    dst_store.reset_counters();

    std::atomic< resync_copy_mode > live{resync_copy_mode::ZERO_TEST};
    Raid1ResyncTask task{bitmap, kOffset, kPage, 256u * Ki, &live};
    std::atomic< bool > injected{false};
    EXPECT_CALL(*src_disk, sync_iov(UBLK_IO_OP_READ, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([&](uint8_t, iovec* iov, uint32_t, off_t addr) -> io_result {
            auto r = src_store.read(iov, addr);
            // Simulate a write arriving mid-copy exactly once so Phase 2 sees the conflict.
            if (!injected.exchange(true)) task.enqueue_write(kBase, kRegion);
            return r;
        });

    task.launch(test_uuid, clean_mirror, dirty_mirror, [] { return true; });

    // yield_count>=1 implies the first sweep's Phase-2 (and thus the downgrade) has run.
    auto const deadline = std::chrono::steady_clock::now() + 2000ms;
    while (task.yield_count() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    task.dequeue_write(kBase, kRegion); // release so the deferred region re-syncs in CHECK

    EXPECT_TRUE(wait_for_bitmap_clean(bitmap));
    EXPECT_GT(dst_store.reads.load(), 0) << "downgrade to CHECK must read the destination on re-sync";
    // The downgrade must be PUBLISHED to the shared mode, not just kept run-local, so a persist or
    // relaunch also resumes in CHECK (review finding 2).
    EXPECT_EQ(resync_copy_mode::CHECK, live.load()) << "ZERO_TEST -> CHECK downgrade must reach the shared mode";
    task.stop();
}

// A dirty run wider than max_io copies in multiple chunks. When an earlier chunk copies (any_copy)
// but a later chunk of the same run has an in-flight write, the Phase-1 skip must advance to the
// next dirty run rather than stall -- and the held chunk still cleans once the write completes.
TEST_F(ResyncCopyModeTest, RunSpanningPhase1SkipAdvancesAfterPartialCopy) {
    constexpr uint32_t kMax = 256u * Ki;
    bitmap->dirty_region(kBase, 2u * kMax); // one contiguous run spanning two max_io chunks

    std::atomic< resync_copy_mode > live{resync_copy_mode::BLIND};
    Raid1ResyncTask task{bitmap, kOffset, kPage, kMax, &live};
    task.enqueue_write(kBase + kMax, kMax); // hold the SECOND chunk in-flight (Phase-1 conflict)
    task.launch(test_uuid, clean_mirror, dirty_mirror, [] { return true; });

    // First chunk copies (any_copy==true); the conflicting second chunk trips the next_dirty advance.
    auto const deadline = std::chrono::steady_clock::now() + 2000ms;
    while (task.yield_count() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    EXPECT_GT(bitmap->dirty_pages(), 0u) << "the in-flight second chunk must stay dirty";

    task.dequeue_write(kBase + kMax, kMax);
    EXPECT_TRUE(wait_for_bitmap_clean(bitmap)) << "the run must finish once the held write completes";
    task.stop();
}

// The mode enum is formatted into the resync-start log; exercise its generated accessors
// (fmt format_as, enum_name, and operator<<).
TEST(Raid1ResyncCopyMode, FormatsAllModes) {
    std::ostringstream oss;
    for (auto m : {resync_copy_mode::BLIND, resync_copy_mode::CHECK, resync_copy_mode::ZERO_TEST}) {
        EXPECT_FALSE(fmt::format("{}", m).empty());
        EXPECT_FALSE(enum_name(m).empty());
        oss << m;
    }
    EXPECT_FALSE(oss.str().empty());
}

// A leg that reads back a valid superblock (existing). `age` lets a peer be age-promoted to
// new_device while remaining populated (sb_was_fresh == false), which must fall back to CHECK.
static std::shared_ptr< ublkpp::TestDisk > make_valid_leg(std::string const& id, bool slot_b, uint64_t age = 0) {
    auto d = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = id, .is_slot_b = slot_b});
    EXPECT_CALL(*d, sync_iov(_, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([slot_b, age](uint8_t op, iovec* iov, uint32_t, off_t) -> io_result {
            if (op == UBLK_IO_OP_READ && iov->iov_base) {
                std::memcpy(iov->iov_base, &normal_superblock, k_page_size);
                auto* sb = static_cast< SuperBlock* >(iov->iov_base);
                if (slot_b) sb->fields.device_b = 1;
                sb->fields.bitmap.age = htobe64(age);
            }
            return static_cast< int >(iov->iov_len);
        });
    return d;
}

// A genuinely-fresh leg: reads back all zeros (no superblock magic) => new_device && sb_was_fresh.
static std::shared_ptr< ublkpp::TestDisk > make_fresh_leg(std::string const& id, bool slot_b) {
    auto d = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = id, .is_slot_b = slot_b});
    EXPECT_CALL(*d, sync_iov(_, _, _, _))
        .Times(AnyNumber())
        .WillRepeatedly([](uint8_t op, iovec* iov, uint32_t, off_t) -> io_result {
            if (op == UBLK_IO_OP_READ && iov->iov_base) std::memset(iov->iov_base, 0, iov->iov_len);
            return static_cast< int >(iov->iov_len);
        });
    return d;
}

// Mode selection at construction (__init_bitmap_and_degraded_route replacement branch). CHECK-vs-
// BLIND keys only on whether the replacement leg still holds data (sb_was_fresh): a data-bearing
// (age-promoted) leg is always CHECK, regardless of assume_clean. assume_clean only upgrades a
// genuinely-fresh leg from BLIND to ZERO_TEST.
TEST(Raid1ModeSelection, ConstructionReplacement) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    // dev_a is canonical at age 16; a valid age-0 dev_b is age-promoted to a stale replacement, a
    // fresh dev_b (no superblock) is a genuinely-fresh replacement.
    auto mode = [&](bool fresh_leg, bool assume_clean) {
        std::shared_ptr< ublkpp::TestDisk > dev_b =
            fresh_leg ? make_fresh_leg("B", true) : make_valid_leg("B", true, 0);
        ublkpp::raid1::Raid1Disk raid(uuid, make_valid_leg("A", false, 16), std::move(dev_b), "", assume_clean);
        return raid.current_resync_mode();
    };
    EXPECT_EQ(resync_copy_mode::CHECK, mode(/*fresh=*/false, /*assume_clean=*/false)); // stale: compare-recover
    EXPECT_EQ(resync_copy_mode::CHECK, mode(false, true));                             // assume_clean irrelevant
    EXPECT_EQ(resync_copy_mode::BLIND, mode(true, false));                             // fresh, no assertion
    EXPECT_EQ(resync_copy_mode::ZERO_TEST, mode(true, true));                          // fresh + asserted zero
}

// Same rule on the swap path. The base array sits at age 16 so a valid age-0 incoming leg is age-
// promoted to a (stale) replacement; a fresh incoming leg has no superblock. Resync is disabled so
// nothing launches -- we only read the selected mode.
TEST(Raid1ModeSelection, SwapReplacement) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    auto mode = [&](bool fresh_leg, bool assume_clean) {
        ublkpp::raid1::Raid1Disk raid(uuid, make_valid_leg("DiskA", false, 16), make_valid_leg("DiskB", true, 16));
        raid.toggle_resync(false);
        std::shared_ptr< ublkpp::TestDisk > incoming =
            fresh_leg ? make_fresh_leg("DiskC", true) : make_valid_leg("DiskC", true, 0);
        raid.swap_device("DiskB", std::move(incoming), assume_clean);
        return raid.current_resync_mode();
    };
    EXPECT_EQ(resync_copy_mode::CHECK, mode(/*fresh=*/false, /*assume_clean=*/false)); // re-added data-bearing leg
    EXPECT_EQ(resync_copy_mode::CHECK, mode(false, true));
    EXPECT_EQ(resync_copy_mode::BLIND, mode(true, false));
    EXPECT_EQ(resync_copy_mode::ZERO_TEST, mode(true, true));
}

// Regression guard for swap_device's `if (swapped)` mode-preservation fix: a rejected swap (here the
// staying leg's SB write fails inside __swap_device, which rolls back to swapped==false) must leave
// the in-progress resync mode untouched. Clobbering it to BLIND would turn a resumable ZERO_TEST
// rebuild into a thin-destroying full copy.
TEST(Raid1ModeSelection, RejectedSwapPreservesMode) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    auto a = make_valid_leg("A", false, 16);
    ublkpp::raid1::Raid1Disk raid(uuid, a, make_fresh_leg("B", true), "", true); // ZERO_TEST rebuild of fresh B
    raid.toggle_resync(false);
    ASSERT_EQ(resync_copy_mode::ZERO_TEST, raid.current_resync_mode());

    // Fail the staying leg's (A) superblock write so __swap_device rolls back (swapped == false).
    EXPECT_CALL(*a, sync_iov(UBLK_IO_OP_WRITE, _, _, ::testing::Eq((off_t)0)))
        .WillRepeatedly([](uint8_t, iovec*, uint32_t, off_t) -> io_result {
            return std::unexpected(std::make_error_condition(std::errc::io_error));
        });
    auto const displaced = raid.swap_device("B", make_fresh_leg("C", true), true);

    EXPECT_EQ("C", displaced->id()) << "a rejected swap returns the incoming device unchanged";
    EXPECT_EQ(resync_copy_mode::ZERO_TEST, raid.current_resync_mode())
        << "a rejected swap must leave the in-progress resync mode intact (the if(swapped) guard)";
}

// A brand-new pair (both legs fresh) cannot assume its legs read identically: without assume_clean
// it pins device_a and schedules an md-style initial BLIND sync; with assume_clean both legs read
// zero -- already identical -- and no initial sync is needed.
TEST(Raid1ModeSelection, FreshPairInitialSync) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    {
        ublkpp::raid1::Raid1Disk raid(uuid, make_fresh_leg("A", false), make_fresh_leg("B", true), "", false);
        raid.toggle_resync(false);
        EXPECT_EQ(resync_copy_mode::BLIND, raid.current_resync_mode());
        EXPECT_GT(raid.replica_states().bytes_to_sync, Gi / 2) << "initial sync must cover the array";
    }
    {
        ublkpp::raid1::Raid1Disk raid(uuid, make_fresh_leg("A", false), make_fresh_leg("B", true), "", true);
        raid.toggle_resync(false);
        EXPECT_EQ(0u, raid.replica_states().bytes_to_sync) << "assume_clean pair needs no initial sync";
    }
}

// When a rebuild completes and the array returns to EITHER, __become_clean resets the copy mode to
// BLIND so a later resync of the now-populated leg never runs ZERO_TEST (which would skip zero-source
// pages over real data -- silent divergence). The direct-task tests bypass __become_clean, so this is
// its only coverage: drive a full Raid1Disk ZERO_TEST rebuild to completion and assert the reset.
TEST(Raid1ModeSelection, BecomeCleanResetsZeroTestToBlind) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    ublkpp::raid1::Raid1Disk raid(uuid, make_valid_leg("A", false, 16), make_fresh_leg("B", true), "", true);
    ASSERT_EQ(resync_copy_mode::ZERO_TEST, raid.current_resync_mode()) << "fresh leg + assume_clean -> ZERO_TEST";
    ASSERT_GT(raid.replica_states().bytes_to_sync, 0u) << "degraded rebuild scheduled";

    raid.toggle_resync(true); // launch the real resync thread
    ASSERT_TRUE(wait_for_clean_state(raid, 5000ms)) << "ZERO_TEST rebuild did not converge";
    raid.toggle_resync(false);
    EXPECT_EQ(resync_copy_mode::BLIND, raid.current_resync_mode())
        << "__become_clean must reset the mode so a later resync of the now-populated leg is never ZERO_TEST";
}

// The public free-function wrappers dispatch to the impl (assume_clean overloads included) and
// return safe defaults for a non-Raid1 disk. Tests that drive Raid1Disk directly bypass both paths.
TEST(Raid1FreeFunctions, DispatchAndNonRaid1Fallback) {
    auto const uuid = boost::uuids::string_generator()(test_uuid);
    auto disk = ublkpp::make_raid1_disk(uuid, make_valid_leg("DiskA", false), make_valid_leg("DiskB", true), "", true);
    ASSERT_NE(nullptr, disk);
    auto* r1 = dynamic_cast< ublkpp::raid1::Raid1Disk* >(disk.get());
    ASSERT_NE(nullptr, r1);
    r1->toggle_resync(false); // keep the swap from relaunching a resync thread

    // Happy-path dispatch through the free functions.
    (void)ublkpp::raid1::replica_states(*disk);
    EXPECT_NE(nullptr, ublkpp::raid1::replicas(*disk).first);
    auto displaced = ublkpp::raid1::swap_device(*disk, "DiskB", make_fresh_leg("DiskC", true), /*assume_clean=*/true);
    EXPECT_EQ("DiskB", displaced->id());

    // Non-Raid1 disk -> safe-default fallbacks.
    auto plain = std::make_shared< ublkpp::TestDisk >(TestParams{.capacity = Gi, .id = "plain"});
    EXPECT_EQ(replica_state::ERROR, ublkpp::raid1::replica_states(*plain).device_a);
    EXPECT_EQ(nullptr, ublkpp::raid1::replicas(*plain).first);
    EXPECT_EQ(plain, ublkpp::raid1::swap_device(*plain, "x", plain, false));
}

} // namespace
