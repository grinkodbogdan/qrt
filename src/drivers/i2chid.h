/* i2chid.h - HID over I2C (Microsoft HIDI2C v1.0) touchscreen driver. */
#pragma once
#include "dwi2c.h"
#include "hidparse.h"

int  i2chid_probe(i2chid_t *h, dwi2c_t *bus, u8 addr, u16 desc_reg);  /* reads + parses descriptors */
int  i2chid_set_power(i2chid_t *h, int on);
/* Read one input report. Returns payload length (0 = nothing pending), <0 on error. */
int  i2chid_read(i2chid_t *h, u8 *buf, int cap);
