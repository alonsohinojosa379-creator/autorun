#ifndef SD_CACHE_TEST_SWITCH_H
#define SD_CACHE_TEST_SWITCH_H

#include <stdint.h>

typedef uint64_t u64;
static inline u64 armGetSystemTick(void) { return 1; }
static inline u64 armTicksToNs( u64 ticks ) { return ticks; }

#endif
