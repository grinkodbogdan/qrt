/* speaker.h - the Venue 8 Pro 5855's speakers: SOF on Intel's audio DSP and the RT5672 codec (speaker.c). */
#pragma once
#include "../kernel/kernel.h"

void speaker_start(void);            /* native mode: load the DSP firmware and start the speaker path (a thread) */
const char *speaker_status(void);
