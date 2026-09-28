/* smp.h - run work on the other CPU cores through UEFI MP Services.
 *
 * The boot core (BSP) hands a job to every application processor (AP) and
 * waits for them.  Job code runs on bare cores: it must not call firmware
 * services, allocate memory or log - pure computation on memory only. */
#pragma once
#include "kernel.h"

typedef void (*smp_job_t)(void *arg, int index, int count);

int  smp_init(void);            /* returns the number of usable worker cores */
int  smp_workers(void);         /* 0 when disabled or unavailable */
void smp_set_enabled(int on);
int  smp_enabled(void);
/* Run job(arg, i, count) for i in 0..count-1 across the APs; any index an
 * AP did not complete is run on the BSP.  Always completes all indices. */
void smp_run(smp_job_t job, void *arg, int count);
