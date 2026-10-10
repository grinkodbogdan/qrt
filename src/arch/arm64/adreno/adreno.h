/*
 * adreno.h - the Adreno 5xx register and packet definitions (generated from Mesa's
 * rules-ng-ng XML by the Linux tree's gen_header.py: adreno_common, adreno_pm4, a5xx;
 * MIT licence, see the files) with what they expect around them.
 */
#ifndef QRT_ADRENO_H
#define QRT_ADRENO_H
#include <stdint.h>
#include <stdbool.h>

#define assert(x) ((void)0)
static inline uint32_t fui(float f) { union { float f; uint32_t u; } v; v.f = f; return v.u; }
static inline uint16_t _mesa_float_to_half(float f) { union { __fp16 h; uint16_t u; } v; v.h = (__fp16)f; return v.u; }

#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a5xx.xml.h"
#undef assert
#endif
