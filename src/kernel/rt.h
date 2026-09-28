/* rt.h - freestanding runtime: memory, strings, formatting, math. */
#pragma once
#include "../efi.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS_I(v) ((v) < 0 ? -(v) : (v))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

void *memset(void *d, int c, usize n);
void *memcpy(void *d, const void *s, usize n);
void *memmove(void *d, const void *s, usize n);
int   memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
void  strlcpy(char *d, const char *s, usize cap);
int   str_icontains(const char *hay, const char *needle);
usize str16len(const c16 *s);
void  str16_to_utf8(char *d, usize cap, const c16 *s);
void  utf8_to_str16(c16 *d, usize cap, const char *s);

int   vfmt(char *buf, usize cap, const char *f, va_list ap);
int   fmt(char *buf, usize cap, const char *f, ...);
void  fmt_bytes(char *buf, usize cap, u64 bytes);

void *kalloc(usize n);          /* zeroed; never returns NULL (panics) */
void  kfree(void *p);
void  panic(const char *msg);
void  klog(const char *f, ...); /* boot log (console + ring buffer) */
const char *klog_line(int i);   /* i-th retained line, NULL past end */

float fsqrt(float x);
float fsin(float x);
float fcos(float x);
#define PI_F 3.14159265f

u32 rand32(void);
