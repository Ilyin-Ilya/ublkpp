cmake_minimum_required (VERSION 3.11)

list(APPEND RAID1_TEST_SRCS
  misc/device_probe.cpp
  misc/device_probe_differ.cpp
  misc/device_probe_exceed.cpp
  misc/open_devices.cpp
  misc/open_for_uring.cpp
  misc/replica_states.cpp
  misc/swap_device.cpp
)
set(RAID1_TEST_SRCS "${RAID1_TEST_SRCS}" PARENT_SCOPE)
