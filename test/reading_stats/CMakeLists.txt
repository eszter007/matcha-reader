add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  stubs/StubDefs.cpp
  ${REPO_ROOT}/src/ReadingStatsStore.cpp
  ${REPO_ROOT}/src/BookStats.cpp
)

# The stubs directory comes first so <HalStorage.h> and <Logging.h> resolve to the host
# stand-ins rather than the firmware headers.
target_include_directories(ReadingStatsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
