/* hda.h - Intel High Definition Audio (hda.c). */
#pragma once
#include "../kernel/kernel.h"
#include "pci.h"

void hda_probe(const pci_dev_t *p);          /* native kernel: set the controller and codecs up, play */
const char *hda_status(void);
