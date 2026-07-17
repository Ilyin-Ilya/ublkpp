#pragma once

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <ublksrv.h>

#include "ublkpp/lib/cqe_state.hpp"
#include "ublkpp/raid.hpp"
#include "raid/raid1/raid1_impl.hpp"
#include "raid/raid1/raid1_superblock.hpp"
#include "raid/tests/raid_test_common.hpp"
#include "tests/mock_ublksrv/mock_ublksrv.hpp"
#include "tests/test_disk.hpp"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Return;
using ::ublkpp::Gi;
using ::ublkpp::io_result;
using ::ublkpp::Ki;
using ::ublkpp::ublk_disk;
using ::ublkpp::test::make_async_iov_action;

static const ublkpp::raid1::SuperBlock async_raid1_superblock = {
    .header = {.magic = {0x53, 0x25, 0xff, 0x0a, 0x34, 0x99, 0x3e, 0xc5, 0x67, 0x3a, 0xc8, 0x17, 0x49, 0xae, 0x1b,
                         0x64},
               .version = htobe16(2),
               .uuid = {0xad, 0xa4, 0x07, 0x37, 0x30, 0xe3, 0x49, 0xfe, 0x99, 0x42, 0x5a, 0x28, 0x7d, 0x71, 0xeb,
                        0x3f}},
    .fields = {.clean_unmount = 1,
               .read_route = static_cast< uint8_t >(ublkpp::raid1::read_route::EITHER),
               .device_b = 0,
               .bitmap = {.resync_mode = 0, ._reserved = {0x00}, .chunk_size = htobe32(32 * Ki), .age = 0}},
    .superbitmap_reserved = {0x00}};

struct AsyncRaid1Fixture : public ::testing::Test {
    static constexpr uint64_t k_disk_cap = 1 * Gi;
    static constexpr std::string_view k_uuid = "ada40737-30e3-49fe-9942-5a287d71eb3f";

    std::shared_ptr< ublkpp::AsyncTestDisk > disk_a, disk_b;
    std::shared_ptr< ublkpp::raid1::Raid1Disk > raid;
    std::unique_ptr< ublkpp::MockUblksrv > mock;

    void SetUp() override {
        using ::testing::AnyNumber;
        using ::testing::StrictMock;

        TestParams const pa{.capacity = k_disk_cap, .id = "DiskA", .is_slot_b = false};
        TestParams const pb{.capacity = k_disk_cap, .id = "DiskB", .is_slot_b = true};
        disk_a = std::make_shared< StrictMock< ublkpp::AsyncTestDisk > >(pa);
        disk_b = std::make_shared< StrictMock< ublkpp::AsyncTestDisk > >(pb);

        EXPECT_CALL(*disk_a, prepare(_, _))
            .Times(AnyNumber())
            .WillRepeatedly(Return(ublkpp::ublk_disk::prepare_result{}));
        EXPECT_CALL(*disk_b, prepare(_, _))
            .Times(AnyNumber())
            .WillRepeatedly(Return(ublkpp::ublk_disk::prepare_result{}));

        EXPECT_CALL(*disk_a, sync_iov(_, _, _, _))
            .Times(AnyNumber())
            .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t, off_t) -> io_result {
                if (op == UBLK_IO_OP_READ && iovecs && iovecs->iov_base)
                    memcpy(iovecs->iov_base, &async_raid1_superblock, ublkpp::raid1::k_page_size);
                return static_cast< int >(iovecs->iov_len);
            });
        EXPECT_CALL(*disk_b, sync_iov(_, _, _, _))
            .Times(AnyNumber())
            .WillRepeatedly([](uint8_t op, iovec* iovecs, uint32_t, off_t) -> io_result {
                if (op == UBLK_IO_OP_READ && iovecs && iovecs->iov_base) {
                    memcpy(iovecs->iov_base, &async_raid1_superblock, ublkpp::raid1::k_page_size);
                    static_cast< ublkpp::raid1::SuperBlock* >(iovecs->iov_base)->fields.device_b = 1;
                }
                return static_cast< int >(iovecs->iov_len);
            });

        ON_CALL(*disk_a, submit_iov(_, _, _, _, _)).WillByDefault(make_async_iov_action());
        ON_CALL(*disk_b, submit_iov(_, _, _, _, _)).WillByDefault(make_async_iov_action());
        EXPECT_CALL(*disk_a, submit_iov(_, _, _, _, _)).Times(AnyNumber()).WillRepeatedly(make_async_iov_action());
        EXPECT_CALL(*disk_b, submit_iov(_, _, _, _, _)).Times(AnyNumber()).WillRepeatedly(make_async_iov_action());

        raid = std::make_shared< ublkpp::raid1::Raid1Disk >(boost::uuids::string_generator()(std::string(k_uuid)),
                                                            disk_a, disk_b);
        mock = std::make_unique< ublkpp::MockUblksrv >(raid);
        // Disable auto-resync AFTER prepare() runs so _resync_enabled=false for test bodies.
        // Tests that explicitly test resync call toggle_resync(true) themselves.
        raid->toggle_resync(false);
    }
};
