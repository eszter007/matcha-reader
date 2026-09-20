add_executable(SideButtonActionsTest SideButtonActionsTest.cpp)

target_include_directories(SideButtonActionsTest PRIVATE ../../src)

target_link_libraries(SideButtonActionsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SideButtonActionsTest)
