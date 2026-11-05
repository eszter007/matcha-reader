add_executable(WordLookupSuruTest
  WordLookupSuruTest.cpp
  ${REPO_ROOT}/lib/Dict/WordLookup.cpp
  ${REPO_ROOT}/lib/Dict/Deinflector.cpp
  ${REPO_ROOT}/lib/Dict/DictIndex.cpp
  ${REPO_ROOT}/lib/Memory/ResidentFiles.cpp
)

# Host stand-ins for <HalStorage.h> and <Logging.h> come first.
target_include_directories(WordLookupSuruTest BEFORE PRIVATE
  ${REPO_ROOT}/test/common/dict_stubs
  ${REPO_ROOT}/lib/Dict
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(WordLookupSuruTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordLookupSuruTest)
