add_executable(LogoArtTest
  LogoArtTest.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/LogoArt.cpp
)

target_include_directories(LogoArtTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LogoArtTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LogoArtTest)
