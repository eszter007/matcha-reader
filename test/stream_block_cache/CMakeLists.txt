add_executable(StreamBlockCacheTest
  StreamBlockCacheTest.cpp
  ${REPO_ROOT}/lib/EpdFont/StreamBlockCache.cpp
)

target_include_directories(StreamBlockCacheTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(StreamBlockCacheTest PRIVATE
  GTest::gtest_main
)

gtest_discover_tests(StreamBlockCacheTest)
