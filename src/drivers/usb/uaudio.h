/* uaudio.h - USB audio playback (uaudio.c): the USB Audio Class 1 and 2 as a sound output. */
#pragma once
#include "usb.h"

int  uaudio_probe(udev_t *d);          /* 1: an audio device (taken, or named as unsupported) */
void uaudio_detach(udev_t *d);         /* the device is going away */
const char *uaudio_status(void);       /* the playing device's name, or NULL */
