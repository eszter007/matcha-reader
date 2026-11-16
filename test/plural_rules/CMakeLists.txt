add_executable(PluralRulesTest
  PluralRulesTest.cpp
)

target_include_directories(PluralRulesTest PRIVATE
  ${REPO_ROOT}/lib/I18n
)

target_link_libraries(PluralRulesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PluralRulesTest)
