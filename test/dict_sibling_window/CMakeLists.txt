add_executable(DictSiblingWindowTest
  DictSiblingWindowTest.cpp
  ${REPO_ROOT}/lib/Dict/DictIndex.cpp
)

# The stubs directory comes first so <HalStorage.h> and <Logging.h> resolve to the host
# stand-ins rather than the firmware headers.
target_include_directories(DictSiblingWindowTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Dict
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(DictSiblingWindowTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictSiblingWindowTest)
