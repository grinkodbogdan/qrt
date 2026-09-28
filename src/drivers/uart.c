/* uart.c - 16550 driver for COM1 (I/O port 0x3F8): polled until the native
 * kernel routes IRQ 4, then receive is interrupt-driven into a ring. */
#include "uart.h"

#define COM1 0x3f8
static int present;

static inline void out8(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8 in8(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

int uart_init(void) {
    /* A missing port reads back 0xff; the scratch register must hold a value. */
    out8(COM1 + 7, 0x5a);
    if (in8(COM1 + 7) != 0x5a) return present = 0;
    out8(COM1 + 7, 0xa5);
    if (in8(COM1 + 7) != 0xa5) return present = 0;
    out8(COM1 + 1, 0x00);          /* no interrupts: polled */
    out8(COM1 + 3, 0x80);          /* DLAB */
    out8(COM1 + 0, 0x01);          /* 115200 baud */
    out8(COM1 + 1, 0x00);
    out8(COM1 + 3, 0x03);          /* 8N1 */
    out8(COM1 + 2, 0xc7);          /* FIFO on, cleared */
    out8(COM1 + 4, 0x03);          /* DTR + RTS */
    return present = 1;
}

int uart_present(void) { return present; }

void uart_putc(char c) {
    if (!present) return;
    for (int i = 0; i < 100000 && !(in8(COM1 + 5) & 0x20); i++) {}
    out8(COM1, (u8)c);
}

void uart_write(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s);
    }
}

static u8 ring[1024];
static volatile u32 head, tail;          /* producer: the IRQ handler; consumer: uart_getc */
static int irq_mode;
static volatile u64 rx_count;

void uart_irq(void *arg) {
    (void)arg;
    while (in8(COM1 + 5) & 1) {
        u8 c = in8(COM1);
        rx_count++;
        u32 h = head;
        if (h - tail < sizeof ring) { ring[h % sizeof ring] = c; __atomic_store_n(&head, h + 1, __ATOMIC_RELEASE); }
    }
}

void uart_irq_enable(void) {
    if (!present) return;
    out8(COM1 + 4, 0x0b);          /* DTR + RTS + OUT2 (gates the IRQ line) */
    irq_mode = 1;
    out8(COM1 + 1, 0x01);          /* interrupt on received data (fires at once if bytes are waiting) */
}

u64 uart_rx_count(void) { return rx_count; }

int uart_getc(void) {
    if (!present) return -1;
    if (irq_mode) {
        u32 t = tail;
        if (t == __atomic_load_n(&head, __ATOMIC_ACQUIRE)) return -1;
        int c = ring[t % sizeof ring];
        tail = t + 1;
        return c;
    }
    if (!(in8(COM1 + 5) & 1)) return -1;
    rx_count++;
    return in8(COM1);
}
