add_executable(StatsDayTest
  StatsDayTest.cpp
)

target_include_directories(StatsDayTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(StatsDayTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsDayTest)
