# Same version as platformio.ini. Header-only; the test serialises fixtures with it.
FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(StatsSyncTest
  StatsSyncTest.cpp
  ${REPO_ROOT}/src/stats/StatsPayload.cpp
  ${REPO_ROOT}/src/stats/DailyStatsParser.cpp
  ${REPO_ROOT}/src/stats/BookListParser.cpp
  ${REPO_ROOT}/src/stats/OverviewParser.cpp
  ${REPO_ROOT}/src/stats/ProgressChangesParser.cpp
  ${REPO_ROOT}/src/stats/BookActivityParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(StatsSyncTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
)

# The checks are plain assert(); keep them live in Release builds.
target_compile_options(StatsSyncTest PRIVATE -UNDEBUG)
target_link_libraries(StatsSyncTest PRIVATE ArduinoJson)

add_test(NAME StatsSyncTest COMMAND StatsSyncTest)
