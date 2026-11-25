add_executable(CssParserTest
  CssParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookFontMap.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ReaderFontScale.cpp
)

target_include_directories(CssParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/AsciiText
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(CssParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserTest)

# The same suite with the PSRAM-only vertical-style memo compiled in.
add_executable(CssParserPsramTest
  CssParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookFontMap.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ReaderFontScale.cpp
)

target_compile_definitions(CssParserPsramTest PRIVATE BOARD_HAS_PSRAM=1)

target_include_directories(CssParserPsramTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/psram_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/AsciiText
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(CssParserPsramTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserPsramTest TEST_PREFIX "Psram.")
