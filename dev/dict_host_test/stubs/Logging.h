#pragma once
#include <cstdio>
#define LOG_DBG(tag, fmt, ...) std::printf("[DBG][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_ERR(tag, fmt, ...) std::fprintf(stderr, "[ERR][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define LOG_INF(tag, fmt, ...) std::printf("[INF][%s] " fmt "\n", tag, ##__VA_ARGS__)
