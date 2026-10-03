/* audio.h - the Venue 8 Pro 5855's sound hardware: which codec is fitted. */
#pragma once
#include "../kernel/kernel.h"

void audio_probe(void);              /* firmware stage: identify the codec on I2C2 */
const char *audio_status(void);      /* one line for System Monitor and the device list */
struct dwi2c;
struct dwi2c *audio_bus(void);       /* I2C2, where the codec answers (NULL if not found) */
void audio_native_resume(void);      /* after the handover: the bus's settings back */
