add_executable(RecentBooksMergeTest
  RecentBooksMergeTest.cpp
)

target_link_libraries(RecentBooksMergeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RecentBooksMergeTest)
