#pragma once
// DictIndex logs only diagnostics (spx load info, OOM skips); the tests assert on lookup
// results, not on log output.
#define LOG_ERR(mod, ...) \
  do {                    \
  } while (0)
#define LOG_DBG(mod, ...) \
  do {                    \
  } while (0)
#define LOG_INF(mod, ...) \
  do {                    \
  } while (0)
