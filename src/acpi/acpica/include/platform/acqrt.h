/*
 * acqrt.h - ACPICA's host definitions for QRT's kernel (Tessera), x86-64, built with
 * clang for a *-windows-gnu target (long is 32 bits there: 64-bit types are spelled out).
 * The kernel has most of the C string functions ACPICA calls; the rest are in
 * src/acpi/osl.c.
 */
#ifndef __ACQRT_H__
#define __ACQRT_H__

#define ACPI_MACHINE_WIDTH          64
#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_DO_WHILE_0
#define ACPI_USE_LOCAL_CACHE
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
#define ACPI_NO_ERROR_MESSAGES_OFF

#define ACPI_CPU_FLAGS              unsigned long long
#define ACPI_SPINLOCK               void *
#define ACPI_SEMAPHORE              void *
#define ACPI_MUTEX_TYPE             ACPI_BINARY_SEMAPHORE

#define ACPI_FLUSH_CPU_CACHE()      __asm__ volatile("wbinvd" ::: "memory")

/* the FACS global lock, as Linux does it (arch/x86/kernel/acpi/boot.c) */
int AcpiOsQrtAcquireGlobalLock(void *facs);
int AcpiOsQrtReleaseGlobalLock(void *facs);
#define ACPI_ACQUIRE_GLOBAL_LOCK(facs, Acq)  ((Acq) = AcpiOsQrtAcquireGlobalLock(facs))
#define ACPI_RELEASE_GLOBAL_LOCK(facs, Pnd)  ((Pnd) = AcpiOsQrtReleaseGlobalLock(facs))

typedef __builtin_va_list va_list;
#ifndef va_start
#define va_start(v, l)  __builtin_va_start(v, l)
#define va_end(v)       __builtin_va_end(v)
#define va_arg(v, l)    __builtin_va_arg(v, l)
#endif
#ifndef va_copy
#define va_copy(d, s)   __builtin_va_copy(d, s)
#endif

void *memset(void *d, int c, unsigned long long n);
void *memcpy(void *d, const void *s, unsigned long long n);
void *memmove(void *d, const void *s, unsigned long long n);
int   memcmp(const void *a, const void *b, unsigned long long n);
unsigned long long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long long n);
char *strchr(const char *s, int c);
char *strstr(const char *hay, const char *needle);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, unsigned long long n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, unsigned long long n);
unsigned long strtoul(const char *s, char **end, int base);

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7f; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }

#endif
