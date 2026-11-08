add_executable(FontObfuscationTest
  FontObfuscationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FontObfuscation.cpp
)

target_include_directories(FontObfuscationTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(FontObfuscationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontObfuscationTest)
