add_executable(RecentPagesTest
  RecentPagesTest.cpp
  ${REPO_ROOT}/src/util/RecentPages.cpp
)

target_include_directories(RecentPagesTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(RecentPagesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RecentPagesTest)
