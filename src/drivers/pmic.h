/* pmic.h - the power-management IC on I2C7 (Cherry Trail), shared with the SoC's P-unit. */
#pragma once
#include "../kernel/kernel.h"

enum { PMIC_NONE, PMIC_CRYSTAL_COVE, PMIC_DOLLAR_COVE_TI, PMIC_WHISKEY_COVE, PMIC_XPOWER };

void pmic_probe(void);              /* firmware stage: find I2C7 and which PMIC answers */
void pmic_native_resume(void);
int  pmic_kind(void);
int  pmic_read(u8 reg, u8 *val);    /* 0 or < 0; holds the P-unit semaphore around the transfer */
int  pmic_write(u8 reg, u8 val);
const char *pmic_status(void);
