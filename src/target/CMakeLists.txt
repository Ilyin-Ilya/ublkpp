cmake_minimum_required (VERSION 3.11)

add_library(ublkpp_tgt OBJECT)
target_sources(ublkpp_tgt PRIVATE
    ublkpp_tgt.cpp
)
target_link_libraries(ublkpp_tgt
    ublk_metrics
    sisl::cache
    ublksrv::ublksrv
)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})
if ((DEFINED ENABLE_TESTS) AND (${ENABLE_TESTS}))
add_subdirectory (tests)
endif()
