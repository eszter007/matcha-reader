add_executable(VerticalPageBreakTest
  VerticalPageBreakTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/VerticalParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Kinsoku.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(VerticalPageBreakTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(VerticalPageBreakTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VerticalPageBreakTest)
