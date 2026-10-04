add_executable(SentenceMiningTest
  SentenceMiningTest.cpp
  ${REPO_ROOT}/lib/SentenceMining/SentenceMiningCsv.cpp
)

target_include_directories(SentenceMiningTest PRIVATE
  ${REPO_ROOT}/lib/SentenceMining
)

target_link_libraries(SentenceMiningTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SentenceMiningTest)
