add_executable(InflectionRulesTest
  InflectionRulesTest.cpp
  ${REPO_ROOT}/src/util/InflectionRules.cpp
  ${REPO_ROOT}/src/util/KoreanInflection.cpp
)

target_include_directories(InflectionRulesTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(InflectionRulesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InflectionRulesTest)
