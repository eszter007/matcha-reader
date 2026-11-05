add_executable(CounterReadingTest
  CounterReadingTest.cpp
  ${REPO_ROOT}/lib/Dict/CounterReading.cpp
)

target_include_directories(CounterReadingTest PRIVATE
  ${REPO_ROOT}/lib/Dict
)

target_link_libraries(CounterReadingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CounterReadingTest)
