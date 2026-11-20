#pragma once
#include <assert.h>
#include <stdlib.h>
#define furi_check(x)   assert(x)
#define furi_assert(x)  assert(x)
#define furi_crash(...) abort()
// Flipper's allocator clears new memory. Mirror that behavior in the SDK code.
static inline void* sim_zero_alloc(size_t size) {
    return calloc(1, size);
}
#define malloc(size) sim_zero_alloc(size)
