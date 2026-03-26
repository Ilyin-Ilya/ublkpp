cmake_minimum_required (VERSION 3.11)

add_library(raid1 OBJECT)
target_sources(raid1 PRIVATE
    raid1.cpp
    raid1_pimpl.cpp
    raid1_superblock.cpp
    bitmap.cpp
    super_bitmap.cpp
)
target_link_libraries(raid1
    isa-l::isa-l
    sisl::cache
    ublksrv::ublksrv
)

if ((DEFINED ENABLE_TESTS) AND (${ENABLE_TESTS}))
add_subdirectory (tests)
endif()
