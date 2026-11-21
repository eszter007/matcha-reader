# Every language but English comes from a pack; the packs are built with the release script.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(HYPHENATION_PACKS ${CMAKE_CURRENT_BINARY_DIR}/hyph)
add_custom_target(HyphenationEvalPacks
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/build_hyphenation_packs.py --out ${HYPHENATION_PACKS}
  COMMENT "Building hyphenation packs for the evaluation"
)

add_executable(HyphenationEvaluationTest
  HyphenationEvaluationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationPack.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
add_dependencies(HyphenationEvaluationTest HyphenationEvalPacks)

target_include_directories(HyphenationEvaluationTest PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(HyphenationEvaluationTest PRIVATE
  HYPHENATION_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  HYPHENATION_PACK_DIR="${HYPHENATION_PACKS}"
)

target_link_libraries(HyphenationEvaluationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HyphenationEvaluationTest)
