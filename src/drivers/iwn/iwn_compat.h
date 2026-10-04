/*
 * iwn_compat.h - just enough of the OpenBSD kernel environment to include OpenBSD's
 * if_iwnreg.h unchanged (register offsets, firmware commands and tables of Intel's
 * 4965/5000/1000/6000/2000 wireless, the generation before iwm's), as iwm_compat.h does.
 */
#pragma once
#include "../../kernel/kernel.h"

typedef u8  uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef i8  int8_t;
typedef i16 int16_t;
typedef i32 int32_t;
typedef i64 int64_t;

#define __packed      __attribute__((packed))
#define __aligned(x)  __attribute__((aligned(x)))

#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define letoh16(x) le16toh(x)
#define letoh32(x) le32toh(x)

#define IEEE80211_ADDR_LEN      6
#define IEEE80211_NWID_LEN      32
#define IEEE80211_TKIP_MICLEN   8
#define EDCA_NUM_AC             4
#define MCLBYTES                2048
#define PCI_MAPREG_START        0x10

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#include "if_iwnreg.h"
#pragma clang diagnostic pop

/* QRT addresses DMA by physical address up to 36 bits on every target (the header
 * chooses by __LP64__, which the *-windows targets do not define) */
#undef IWN_LOADDR
#undef IWN_HIADDR
#define IWN_LOADDR(pa) ((uint32_t)(pa))
#define IWN_HIADDR(pa) ((uint32_t)(((u64)(pa) >> 32) & 0xf))
