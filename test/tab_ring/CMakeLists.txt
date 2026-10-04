add_executable(TabRingTest TabRingTest.cpp ../../src/components/TabRing.cpp)
target_include_directories(TabRingTest PRIVATE stubs ../../src)
target_link_libraries(TabRingTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TabRingTest)
