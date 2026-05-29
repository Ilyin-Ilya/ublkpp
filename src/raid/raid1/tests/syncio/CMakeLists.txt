cmake_minimum_required (VERSION 3.11)

list(APPEND RAID1_TEST_SRCS
  syncio/double_fail.cpp
  syncio/fail_sb_update.cpp
  syncio/read_fail_both.cpp
  syncio/read_fail_deva.cpp
  syncio/read_fail_degraded_dirty.cpp
  syncio/write_backup_fail_degrade_fail.cpp
  syncio/write_clears_unavail.cpp
  syncio/write_degraded_dirty_skips_backup.cpp
  syncio/write_fail_deva.cpp
  syncio/write_fail_devb.cpp
  syncio/become_clean_sb_fail.cpp
)
set(RAID1_TEST_SRCS "${RAID1_TEST_SRCS}" PARENT_SCOPE)
