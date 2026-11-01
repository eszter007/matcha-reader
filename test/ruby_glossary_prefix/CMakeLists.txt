add_executable(RubyGlossaryPrefixTest
  RubyGlossaryPrefixTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/RubyGlossary.cpp
)

# Host stand-ins for <Arduino.h>, <HalStorage.h> and <Logging.h> come first.
target_include_directories(RubyGlossaryPrefixTest BEFORE PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(RubyGlossaryPrefixTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RubyGlossaryPrefixTest)
