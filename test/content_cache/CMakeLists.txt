add_executable(ContentCacheTest
  ContentCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ContentCache.cpp
)
target_include_directories(ContentCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub
)
target_link_libraries(ContentCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ContentCacheTest)
