#include "test_raid1_common.hpp"

TEST(Raid1, PickSuper) {
    {
        auto deva_sb = ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                                 .fields = {.clean_unmount = 0,
                                                            .read_route = 0,
                                                            .device_b = 0,
                                                            .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = 0}},
                                                 .superbitmap_reserved = {0}};
        auto devb_sb = ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                                 .fields = {.clean_unmount = 0,
                                                            .read_route = 0,
                                                            .device_b = 0,
                                                            .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = 0}},
                                                 .superbitmap_reserved = {0}};
        auto choice = ublkpp::raid1::pick_superblock(&deva_sb, &devb_sb);
        EXPECT_EQ(choice, &deva_sb);
        EXPECT_EQ((ublkpp::raid1::read_route)choice->fields.read_route, ublkpp::raid1::read_route::EITHER);
    }
    {
        auto deva_sb = ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                                 .fields = {.clean_unmount = 0,
                                                            .read_route = 0,
                                                            .device_b = 0,
                                                            .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = 0}},
                                                 .superbitmap_reserved = {0}};
        auto devb_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 0,
                                                 .read_route = 0,
                                                 .device_b = 1,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(1)}},
                                      .superbitmap_reserved = {0}};
        auto choice = ublkpp::raid1::pick_superblock(&deva_sb, &devb_sb);
        EXPECT_EQ(choice, &devb_sb);
        EXPECT_EQ((ublkpp::raid1::read_route)choice->fields.read_route, ublkpp::raid1::read_route::DEVB);
    }
    {
        auto deva_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 0,
                                                 .read_route = 0,
                                                 .device_b = 0,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(1)}},
                                      .superbitmap_reserved = {0}};
        auto devb_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 1,
                                                 .read_route = 0,
                                                 .device_b = 1,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(1)}},
                                      .superbitmap_reserved = {0}};
        auto choice = ublkpp::raid1::pick_superblock(&deva_sb, &devb_sb);
        EXPECT_EQ(choice, &devb_sb);
        EXPECT_EQ((ublkpp::raid1::read_route)choice->fields.read_route, ublkpp::raid1::read_route::EITHER);
    }
    {
        auto deva_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 0,
                                                 .read_route = 0,
                                                 .device_b = 0,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(2)}},
                                      .superbitmap_reserved = {0}};
        auto devb_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 1,
                                                 .read_route = 0,
                                                 .device_b = 1,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(1)}},
                                      .superbitmap_reserved = {0}};
        auto choice = ublkpp::raid1::pick_superblock(&deva_sb, &devb_sb);
        EXPECT_EQ(choice, &deva_sb);
        EXPECT_EQ((ublkpp::raid1::read_route)choice->fields.read_route, ublkpp::raid1::read_route::DEVA);
    }
    {
        auto deva_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 1,
                                                 .read_route = 0,
                                                 .device_b = 0,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(2)}},
                                      .superbitmap_reserved = {0}};
        auto devb_sb =
            ublkpp::raid1::SuperBlock{.header = {.magic = {0}, .version = 0, .uuid = {0}},
                                      .fields = {.clean_unmount = 0,
                                                 .read_route = 0,
                                                 .device_b = 1,
                                                 .bitmap = {._reserved = {0x00}, .chunk_size = 0, .age = htobe64(2)}},
                                      .superbitmap_reserved = {0}};
        auto choice = ublkpp::raid1::pick_superblock(&deva_sb, &devb_sb);
        EXPECT_EQ(choice, &deva_sb);
        EXPECT_EQ((ublkpp::raid1::read_route)choice->fields.read_route, ublkpp::raid1::read_route::EITHER);
    }
}
