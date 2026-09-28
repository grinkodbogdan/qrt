/* uart.h - 16550 serial port (COM1): kernel log output and test input. */
#pragma once
#include "../kernel/kernel.h"

int  uart_init(void);          /* 1 if a UART answers at COM1 */
int  uart_present(void);
void uart_putc(char c);
void uart_write(const char *s);
int  uart_getc(void);          /* -1 if nothing received */
