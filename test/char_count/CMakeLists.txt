add_executable(CharCountTest
  CharCountTest.cpp
)

target_link_libraries(CharCountTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CharCountTest)
