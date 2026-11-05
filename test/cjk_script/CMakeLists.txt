add_executable(CjkScriptTest
  CjkScriptTest.cpp
)

target_include_directories(CjkScriptTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(CjkScriptTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CjkScriptTest)
