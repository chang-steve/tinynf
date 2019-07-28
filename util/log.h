#pragma once

#if LOG_LEVEL > 0
#include <stdio.h>

// Include this here so that logging can use PRIu32 and friends
#include <inttypes.h>

#define _TN_PRINT(categ, ...) printf("[%5s] ", #categ); printf(__VA_ARGS__); printf("\n"); fflush(stdout);
#endif

#if LOG_LEVEL >= 2
#define TN_DEBUG(...) _TN_PRINT(DEBUG, __VA_ARGS__)
#else
#define TN_DEBUG(...)
#endif

#if LOG_LEVEL >= 1
#define TN_INFO(...) _TN_PRINT(INFO, __VA_ARGS__)
#else
#define TN_INFO(...)
#endif