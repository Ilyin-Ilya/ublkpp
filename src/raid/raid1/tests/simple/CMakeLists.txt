cmake_minimum_required (VERSION 3.11)

list(APPEND RAID1_TEST_SRCS
  simple/discard.cpp
  simple/idle_and_collect.cpp
  simple/syncio.cpp
  simple/read.cpp
  simple/write.cpp
)
set(RAID1_TEST_SRCS "${RAID1_TEST_SRCS}" PARENT_SCOPE)
