cmake_minimum_required (VERSION 3.11)

enable_testing()
find_package(GTest QUIET REQUIRED)

add_compile_options(-Wno-error -pedantic)
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND RAID0_TEST_SRCS
    device_probe.cpp
    device_probe_differ.cpp
    discard_merge.cpp
    discard_retry.cpp
    open_devices.cpp
    raid0_impl_tests.cpp
    split_retry.cpp
    split_write.cpp
    tuple_tests.cpp
)

add_subdirectory (simple)
add_subdirectory (superblock)

add_library(raid0_tests OBJECT)
target_sources(raid0_tests PRIVATE
    ${RAID0_TEST_SRCS}
)
target_link_libraries (raid0_tests
  GTest::gmock
  sisl::cache
  ublksrv::ublksrv
)

add_executable(test_raid0)
target_sources(test_raid0 PRIVATE
  raid0_test.cpp
  $<TARGET_OBJECTS:raid0_tests>
  $<TARGET_OBJECTS:logging>
  $<TARGET_OBJECTS:raid0>
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:ublk_metrics>
)
target_link_libraries (test_raid0
  GTest::gmock
  sisl::cache
)
add_test(NAME Raid0Test COMMAND test_raid0 -cv 2)
