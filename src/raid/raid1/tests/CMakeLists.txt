cmake_minimum_required (VERSION 3.11)
enable_testing()

find_package(GTest QUIET REQUIRED)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

list(APPEND RAID1_TEST_SRCS
  raid1_test.cpp
)

add_subdirectory (asyncio)
add_subdirectory (bitmap)
add_subdirectory (concurrency)
add_subdirectory (region_tracker)
add_subdirectory (resync)
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
  mock_ublksrv
  isa-l::isa-l
  sisl::cache
  ublksrv::ublksrv
  $<$<PLATFORM_ID:Linux>:atomic>
)
add_test(NAME Raid1Test COMMAND test_raid1 -cv warning)
# Run ZeroResyncLevelThrows in isolation with resync_level=0 so the global option is set before
# any MirrorDevice construction; --gtest_filter ensures only this test runs in that invocation.
add_test(NAME Raid1ZeroResyncLevelThrows
  COMMAND test_raid1 --resync_level=0 --gtest_filter=Raid1.ZeroResyncLevelThrows -cv warning)

# Set TSAN suppression file for lock-free read path false positives
if ((DEFINED THREAD_SANITIZER_ON) AND (${THREAD_SANITIZER_ON}))
  set_tests_properties(Raid1Test PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.supp"
  )
endif()
