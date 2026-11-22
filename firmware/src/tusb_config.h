/* Copyright 2026, FreeTouch */
/* tinyusb configuration for the FreeTouch dongle (nice!nano).
 * One CDC-ACM port while the app runs, so data streams live and the board can be sent back to the bootloader. */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_NRF5X
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

/* nrfx 3.5 is the v2 driver path; pinned because the MDK version heuristic has known collisions. */
#define CFG_TUD_NRF_NRFX_VERSION 2

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE  64

#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  2048
/* Must stay 64: it also sizes the OUT endpoint, and 512 broke every host
 * command. Throughput comes from servicing USB while waiting for each poll. */
#define CFG_TUD_CDC_EP_BUFSIZE  64
/* IN may span several 64-byte USB packets; OUT must stay one packet. */
#define CFG_TUD_CDC_TX_EPSIZE   512

#endif
