cmake_minimum_required (VERSION 3.11)

enable_testing()
find_package(GTest QUIET REQUIRED)

add_compile_options(-Wno-unused-result -Wno-maybe-uninitialized)

add_executable(test_logging)
target_sources(test_logging PRIVATE
   test_logging.cpp
  $<TARGET_OBJECTS:logging>
  $<TARGET_OBJECTS:ublk_disk>
  $<TARGET_OBJECTS:ublk_metrics>
)
target_link_libraries (test_logging
  GTest::gmock
  sisl::cache
  ublksrv::ublksrv
)
add_test(NAME LoggingTest COMMAND test_logging -cv 2 --log_mods ublksrv:0)
