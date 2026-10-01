/* audio.h - the Venue 8 Pro 5855's sound hardware: which codec is fitted. */
#pragma once
#include "../kernel/kernel.h"

void audio_probe(void);              /* firmware stage: identify the codec on I2C2 */
const char *audio_status(void);      /* one line for System Monitor and the device list */
