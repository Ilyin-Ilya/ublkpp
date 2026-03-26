cmake_minimum_required (VERSION 3.11)

list(APPEND RAID1_TEST_SRCS
  bitmap/become_clean.cpp
  bitmap/calc_regions.cpp
  bitmap/clean_bitmap.cpp
  bitmap/cross_page.cpp
  bitmap/init_bitmap.cpp
  bitmap/is_dirty.cpp
  bitmap/load_bitmap.cpp
  #bitmap/load_fail.cpp
  bitmap/multiword.cpp
  bitmap/next_dirty.cpp
  bitmap/roundtrip_sync_load.cpp
  bitmap/super_bitmap_test.cpp
  bitmap/sync_to_batching.cpp
  bitmap/sync_to_crash_scenarios.cpp
  bitmap/update_fail.cpp
)
set(RAID1_TEST_SRCS "${RAID1_TEST_SRCS}" PARENT_SCOPE)
