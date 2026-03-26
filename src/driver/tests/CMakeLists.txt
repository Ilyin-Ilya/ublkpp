cmake_minimum_required (VERSION 3.11)

enable_testing()
find_package(GTest QUIET REQUIRED)

add_executable(test_fsdisk)
target_sources(test_fsdisk PRIVATE
   test_fsdisk.cpp test_fs_disk_impl.cpp
  $<TARGET_OBJECTS:fs_disk>
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:ublk_metrics>
  $<TARGET_OBJECTS:logging>
)
target_link_libraries (test_fsdisk
  GTest::gmock
  sisl::cache
  ublksrv::ublksrv
)
add_test(NAME FSDiskTest COMMAND test_fsdisk -cv 2)
