#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n, c) malloc(n)
#define heap_caps_calloc(a, b, c) calloc(a, b)
#define heap_caps_realloc(p, n, c) realloc(p, n)
