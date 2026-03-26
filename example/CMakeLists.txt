cmake_minimum_required(VERSION 3.11)

find_package(iomgr REQUIRED)

if(${libiscsi_FOUND})
    add_compile_options(-DHAVE_ISCSI)
endif()
if(${homeblocks_FOUND})
    add_compile_options(-DHAVE_HOMEBLOCKS)
endif()

add_executable(ublkpp_disk ublkpp_disk.cpp)
target_link_libraries(ublkpp_disk 
    iomgr::iomgr
    ublkpp
)
