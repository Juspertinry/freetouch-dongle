/* Copyright 2026, FreeTouch */
/* Startup for the USB build: a full vector table so USBD (IRQ 39) has a slot,
 * plus the libc functions tinyusb links against. */
#include <stdint.h>
#include <stddef.h>
#include "nrf52.h"

extern uint32_t _etext, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);
void dcd_int_handler(uint8_t rhport);

/* nrfx_coredep's busy-wait delay scales by this. */
uint32_t SystemCoreClock = 64000000u;

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char) c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) { while (n--) *p++ = *q++; }
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void reset_handler(void)
{
    uint32_t *src = &_etext, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; dst++) *dst = 0;
    SCB_VTOR = 0x00026000UL;
    main();
    for (;;) { }
}

static void fatal(void) { for (;;) { } }
static void unused_irq(void) { }

void USBD_IRQHandler(void) { dcd_int_handler(0); }

/* 16 core vectors + 48 external IRQs on the nRF52840. */
__attribute__((section(".vectors"), used))
void (* const vectors[64])(void) = {
    (void (*)(void)) &_estack,
    reset_handler,
    fatal, fatal,                       /* NMI, HardFault */
    fatal, fatal, fatal,                /* MemManage, BusFault, UsageFault */
    0, 0, 0, 0,
    fatal,                              /* SVC */
    0, 0,
    fatal,                              /* PendSV */
    fatal,                              /* SysTick */
    /* external IRQs 0..47 */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,   /* 0-7   */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,   /* 8-15  */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,   /* 16-23 */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,   /* 24-31 */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,               /* 32-38 */
    USBD_IRQHandler,                                                                                  /* 39    */
    unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq, unused_irq,   /* 40-47 */
};
