cmake_minimum_required (VERSION 3.11)

add_library(fs_disk OBJECT)
target_sources(fs_disk PRIVATE
    fs_disk.cpp
)
target_link_libraries(fs_disk
    sisl::cache
    ublksrv::ublksrv
)

if(${libiscsi_FOUND})
  add_library(iscsi_disk OBJECT)
  target_sources(iscsi_disk PRIVATE
      iscsi_disk.cpp
  )
  target_link_libraries(iscsi_disk
      sisl::cache
      ublksrv::ublksrv
      libiscsi::libiscsi
  )
endif()

if (${homeblocks_FOUND})
  add_library(homeblk_disk OBJECT)
  target_sources(homeblk_disk PRIVATE
      homeblk_disk.cpp
  )
  target_link_libraries(homeblk_disk
      sisl::cache
      ublksrv::ublksrv
      homeblocks::homeblocks
  )
endif()

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

if ((DEFINED ENABLE_TESTS) AND (${ENABLE_TESTS}))
add_subdirectory (tests)
endif()
