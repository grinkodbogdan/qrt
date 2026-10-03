/* ish.h - Cherry Trail's Integrated Sensor Hub: accelerometer and ambient light (ish.c). */
#pragma once
#include "../kernel/kernel.h"

void ish_start(void);                 /* native mode, Venue: bring the hub up and switch the sensors on (a thread) */
const char *ish_status(void);
int  ish_orientation(void);           /* 0..3 (the shell's rotation that keeps the picture upright), -1 unknown */
int  ish_lux(void);                   /* ambient light, -1 if none */
void ish_accel(int out[3]);           /* the last raw acceleration */
