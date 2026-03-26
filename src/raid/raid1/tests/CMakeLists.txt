cmake_minimum_required (VERSION 3.11)
enable_testing()

find_package(GTest QUIET REQUIRED)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND RAID1_TEST_SRCS
  raid1_test.cpp
)

add_subdirectory (bitmap)
add_subdirectory (failures)
add_subdirectory (misc)
add_subdirectory (retry)
add_subdirectory (simple)
add_subdirectory (superblock)
add_subdirectory (syncio)

add_executable(test_raid1)
target_sources(test_raid1 PRIVATE
  ${RAID1_TEST_SRCS}
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:ublk_metrics>
  $<TARGET_OBJECTS:logging>
  $<TARGET_OBJECTS:raid1>
)
target_link_libraries (test_raid1
  GTest::gmock
  isa-l::isa-l
  sisl::cache
  ublksrv::ublksrv
)
add_test(NAME Raid1Test COMMAND test_raid1 -cv 2)
