# I18nStrings.cpp / I18nKeys.h are generated from the translation YAML and gitignored, so a fresh
# checkout has neither. The firmware build generates them via PlatformIO; this target generates
# its own copy into the build tree so the test never depends on a prior firmware build.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(I18N_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/i18n)
file(GLOB I18N_TRANSLATIONS ${REPO_ROOT}/lib/I18n/translations/*.yaml)

add_custom_command(
  OUTPUT ${I18N_GEN_DIR}/I18nStrings.cpp ${I18N_GEN_DIR}/I18nStrings.h ${I18N_GEN_DIR}/I18nKeys.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${I18N_GEN_DIR}
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py
          ${REPO_ROOT}/lib/I18n/translations ${I18N_GEN_DIR}
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${I18N_TRANSLATIONS}
  COMMENT "Generating i18n tables for ReadingStatsTest"
  VERBATIM
)

add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
  stubs/StubDefs.cpp
  ${REPO_ROOT}/src/ReadingStatsStore.cpp
  ${REPO_ROOT}/src/BookStats.cpp
  ${REPO_ROOT}/lib/I18n/I18n.cpp
  ${I18N_GEN_DIR}/I18nStrings.cpp
)

# The stubs directory comes first so <HalStorage.h> and <Logging.h> resolve to the host
# stand-ins rather than the firmware headers.
target_include_directories(ReadingStatsTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${I18N_GEN_DIR}
  ${REPO_ROOT}/lib/I18n
  # makeUniqueNoThrow, used by the language-pack loader. Host-portable as written.
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsTest)
