add_executable(ReadingPaceTest
  ReadingPaceTest.cpp
  ${REPO_ROOT}/test/reading_stats/stubs/StubDefs.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingPace.cpp
)

# Reuses the reading-stats host stand-ins for <HalStorage.h> and <Logging.h>.
target_include_directories(ReadingPaceTest BEFORE PRIVATE
  ${REPO_ROOT}/test/reading_stats/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReadingPaceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingPaceTest)
