/*
 * iwm_compat.h - just enough of the OpenBSD kernel environment to include
 * OpenBSD's if_iwmreg.h unchanged (register offsets and firmware structures
 * of Intel 7000/8000/9000 wireless), so they are not re-typed by hand.
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
typedef u64 bus_addr_t;

#define __packed      __attribute__((packed))
#define __aligned(x)  __attribute__((aligned(x)))

/* x86 is little endian: the firmware's byte order */
#define htole16(x) ((uint16_t)(x))
#define htole32(x) ((uint32_t)(x))
#define htole64(x) ((uint64_t)(x))
#define le16toh(x) ((uint16_t)(x))
#define le32toh(x) ((uint32_t)(x))
#define le64toh(x) ((uint64_t)(x))
#define letoh16(x) le16toh(x)
#define letoh32(x) le32toh(x)
#define letoh64(x) le64toh(x)
#define le16_to_cpup(p) (le16toh(*(const uint16_t *)(p)))
#define le32_to_cpup(p) (le32toh(*(const uint32_t *)(p)))

#define ETHER_ADDR_LEN      6
#define IEEE80211_NWID_LEN  32
#define DMA_BIT_MASK(n)     (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

struct ieee80211_frame {
    uint8_t i_fc[2];
    uint8_t i_dur[2];
    uint8_t i_addr1[6];
    uint8_t i_addr2[6];
    uint8_t i_addr3[6];
    uint8_t i_seq[2];
} __packed;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "if_iwmreg.h"
#pragma clang diagnostic pop

/* from OpenBSD's if_iwmvar.h */
#define IWM_UCODE_SECT_MAX      16
#define IWM_FWDMASEGSZ_8000     (320 * 1024)
#define IWM_TX_RING_COUNT       256
#define IWM_RX_RING_COUNT       256
#define IWM_RBUF_SIZE           4096
#define IWM_NUM_PAPD_CH_GROUPS  9
#define IWM_NUM_TXP_CH_GROUPS   9
#define IWM_INIT_COMPLETE       0x01
#define IWM_CALIB_COMPLETE      0x02
#define IWM_STATION_ID          0
#define IWM_AUX_STA_ID          1
#define IWM_CMD_RESP_MAX        4096
#define IWM_SILICON_C_STEP      2
#define IWM_POWER_KEEP_ALIVE_PERIOD_SEC 25

/* the register macros in if_iwmreg.h assume bus_space; QRT maps BAR0 directly */
#undef IWM_READ
#undef IWM_WRITE
#undef IWM_WRITE_1
#undef IWM_SETBITS
#undef IWM_CLRBITS
#undef IWM_BARRIER_WRITE
#undef IWM_BARRIER_READ_WRITE
