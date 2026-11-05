add_executable(DictSiblingWindowTest
  DictSiblingWindowTest.cpp
  ${REPO_ROOT}/lib/Dict/DictIndex.cpp
  ${REPO_ROOT}/lib/Memory/ResidentFiles.cpp
)

# The stubs directory comes first so <HalStorage.h> and <Logging.h> resolve to the host
# stand-ins rather than the firmware headers.
target_include_directories(DictSiblingWindowTest BEFORE PRIVATE
  ${REPO_ROOT}/test/common/dict_stubs
  ${REPO_ROOT}/lib/Dict
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(DictSiblingWindowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictSiblingWindowTest)
