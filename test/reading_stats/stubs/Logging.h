#pragma once
// The stores log only diagnostics; tests assert on behaviour, not on log output.
#define LOG_ERR(mod, ...) \
  do {                    \
  } while (0)
#define LOG_DBG(mod, ...) \
  do {                    \
  } while (0)
#define LOG_INF(mod, ...) \
  do {                    \
  } while (0)
