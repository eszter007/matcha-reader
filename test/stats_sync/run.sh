#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir="${TMPDIR:-/tmp}/crosspoint-stats-sync-tests"
json_dir="${ARDUINOJSON_DIR:-.pio/libdeps/default/ArduinoJson/src}"
mkdir -p "$build_dir"
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -I"$json_dir" -Isrc \
  test/stats_sync/StatsSyncTest.cpp src/stats/StatsPayload.cpp -o "$build_dir/StatsSyncTest"
"$build_dir/StatsSyncTest"
