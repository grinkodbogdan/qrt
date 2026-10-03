/* the few kernel definitions Linux's SOF IPC headers need, for tests/test_speaker.c */
#pragma once
#include <stdint.h>
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef int8_t __s8; typedef int16_t __s16; typedef int32_t __s32;
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#ifndef BIT
#define BIT(n) (1U << (n))
#endif
#define __DECLARE_FLEX_ARRAY(T, name) struct { struct { } __empty_##name; T name[]; }
