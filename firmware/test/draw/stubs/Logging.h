#pragma once

#include <Arduino.h>

// Errors go to stderr. X4_HOST_LOG=1 in the environment adds info lines, =2 debug lines.
void hostLog(int level, const char* tag, const char* origin, const char* format, ...);

#define LOG_ERR(origin, ...) hostLog(0, "ERR", origin, __VA_ARGS__)
#define LOG_INF(origin, ...) hostLog(1, "INF", origin, __VA_ARGS__)
#define LOG_DBG(origin, ...) hostLog(2, "DBG", origin, __VA_ARGS__)
