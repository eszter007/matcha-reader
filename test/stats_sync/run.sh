#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir="${TMPDIR:-/tmp}/crosspoint-stats-sync-tests"
json_dir="${ARDUINOJSON_DIR:-.pio/libdeps/default/ArduinoJson/src}"
mkdir -p "$build_dir"
sax_dir=freeink-sdk/libs/network/JsonSax
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -I"$json_dir" -I"$sax_dir/include" -Isrc \
  test/stats_sync/StatsSyncTest.cpp src/stats/StatsPayload.cpp src/stats/DailyStatsParser.cpp src/stats/BookListParser.cpp \
  "$sax_dir/src/StreamingJsonParser.cpp" -o "$build_dir/StatsSyncTest"
"$build_dir/StatsSyncTest"
