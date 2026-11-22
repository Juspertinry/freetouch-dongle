/* Copyright 2026, FreeTouch */
/* Minimal nRF52840 register defs for the FreeTouch dongle (nice!nano).
 * Not the SDK, since its headers assume an enabled SoftDevice; differs from the nRF51 in 4 KB flash pages and a VTOR. */
#ifndef NRF52_H
#define NRF52_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* --- RADIO 0x40001000 --------------------------------------------------- */
#define RADIO_BASE            0x40001000UL
#define RADIO_TASKS_TXEN      REG(RADIO_BASE + 0x000)
#define RADIO_TASKS_RXEN      REG(RADIO_BASE + 0x004)
#define RADIO_TASKS_START     REG(RADIO_BASE + 0x008)
#define RADIO_TASKS_STOP      REG(RADIO_BASE + 0x00C)
#define RADIO_TASKS_DISABLE   REG(RADIO_BASE + 0x010)
#define RADIO_TASKS_RSSISTART REG(RADIO_BASE + 0x014)
#define RADIO_EVENTS_READY    REG(RADIO_BASE + 0x100)
#define RADIO_EVENTS_ADDRESS  REG(RADIO_BASE + 0x104)
#define RADIO_EVENTS_PAYLOAD  REG(RADIO_BASE + 0x108)
#define RADIO_EVENTS_END      REG(RADIO_BASE + 0x10C)
#define RADIO_EVENTS_DISABLED REG(RADIO_BASE + 0x110)
#define RADIO_EVENTS_RSSIEND  REG(RADIO_BASE + 0x11C)
#define RADIO_SHORTS          REG(RADIO_BASE + 0x200)
#define RADIO_CRCSTATUS       REG(RADIO_BASE + 0x400)
#define RADIO_RXMATCH         REG(RADIO_BASE + 0x408)
#define RADIO_RXCRC           REG(RADIO_BASE + 0x40C)
#define RADIO_PACKETPTR       REG(RADIO_BASE + 0x504)
#define RADIO_FREQUENCY       REG(RADIO_BASE + 0x508)
#define RADIO_TXPOWER         REG(RADIO_BASE + 0x50C)
#define RADIO_MODE            REG(RADIO_BASE + 0x510)
#define RADIO_PCNF0           REG(RADIO_BASE + 0x514)
#define RADIO_PCNF1           REG(RADIO_BASE + 0x518)
#define RADIO_BASE0           REG(RADIO_BASE + 0x51C)
#define RADIO_BASE1           REG(RADIO_BASE + 0x520)
#define RADIO_PREFIX0         REG(RADIO_BASE + 0x524)
#define RADIO_PREFIX1         REG(RADIO_BASE + 0x528)
#define RADIO_TXADDRESS       REG(RADIO_BASE + 0x52C)
#define RADIO_RXADDRESSES     REG(RADIO_BASE + 0x530)
#define RADIO_CRCCNF          REG(RADIO_BASE + 0x534)
#define RADIO_CRCPOLY         REG(RADIO_BASE + 0x538)
#define RADIO_CRCINIT         REG(RADIO_BASE + 0x53C)
#define RADIO_RSSISAMPLE      REG(RADIO_BASE + 0x548)
#define RADIO_STATE           REG(RADIO_BASE + 0x550)
#define RADIO_DATAWHITEIV     REG(RADIO_BASE + 0x554)
#define RADIO_MODECNF0        REG(RADIO_BASE + 0x650)
#define RADIO_POWER           REG(RADIO_BASE + 0xFFC)

/* These two keep their nRF51 values (BLE_1MBIT moved), so the CV1 config transfers unchanged. */
#define RADIO_MODE_NRF_1MBIT  0
#define RADIO_MODE_NRF_2MBIT  1

/* --- CLOCK 0x40000000 --------------------------------------------------- */
#define CLOCK_BASE                0x40000000UL
#define CLOCK_TASKS_HFCLKSTART    REG(CLOCK_BASE + 0x000)
#define CLOCK_EVENTS_HFCLKSTARTED REG(CLOCK_BASE + 0x100)
#define CLOCK_HFCLKSTAT           REG(CLOCK_BASE + 0x40C)

/* --- NVMC 0x4001E000 ---------------------------------------------------- */
#define NVMC_READY     REG(0x4001E400UL)
#define NVMC_CONFIG    REG(0x4001E504UL)
#define NVMC_ERASEPAGE REG(0x4001E508UL)
#define FLASH_PAGE     4096u

/* --- FICR 0x10000000 : silicon self-identification ---------------------- */
#define FICR_DEVICEID0    REG(0x10000060UL)
#define FICR_DEVICEID1    REG(0x10000064UL)
#define FICR_INFO_PART    REG(0x10000100UL)
#define FICR_INFO_VARIANT REG(0x10000104UL)
#define FICR_INFO_PACKAGE REG(0x10000108UL)
#define FICR_INFO_RAM     REG(0x1000010CUL)
#define FICR_INFO_FLASH   REG(0x10000110UL)

/* --- core --------------------------------------------------------------- */
#define SCB_VTOR   REG(0xE000ED08UL)
#define NVIC_ICER0 REG(0xE000E180UL)
#define NVIC_ICPR0 REG(0xE000E280UL)

#endif

/* --- TIMER0 0x40008000 : slot clock ------------------------------------- */
#define TIMER0_BASE           0x40008000UL
#define TIMER0_TASKS_START    REG(TIMER0_BASE + 0x000)
#define TIMER0_TASKS_CLEAR    REG(TIMER0_BASE + 0x00C)
#define TIMER0_TASKS_CAPTURE0 REG(TIMER0_BASE + 0x040)
#define TIMER0_EVENTS_COMPARE0 REG(TIMER0_BASE + 0x140)
#define TIMER0_MODE           REG(TIMER0_BASE + 0x504)
#define TIMER0_BITMODE        REG(TIMER0_BASE + 0x508)
#define TIMER0_PRESCALER      REG(TIMER0_BASE + 0x510)
#define TIMER0_CC0            REG(TIMER0_BASE + 0x540)

/* RADIO extras the CV1 PHY needs that the smoke test did not */
#define RADIO_EVENTS_DEVMATCH REG(RADIO_BASE + 0x114)
#define RADIO_DACNF           REG(RADIO_BASE + 0x640)
#define RADIO_TEST            REG(RADIO_BASE + 0x540)

/* Factory radio address. hmd_rf builds the headset address from these, so no headset identity has to be cloned. */
#define FICR_DEVICEADDR0  REG(0x100000A4UL)
#define FICR_DEVICEADDR1  REG(0x100000A8UL)
#define RADIO_INTENSET    REG(RADIO_BASE + 0x304)
#define RADIO_INTENCLR    REG(RADIO_BASE + 0x308)

/* --- self-reboot into the UF2 bootloader --------------------------------
 * GPREGRET survives reset and 0x57 keeps the Adafruit bootloader in UF2 mode, so no shorting the nice!nano's reset pads. */
#define POWER_GPREGRET  REG(0x4000051CUL)
#define DFU_MAGIC_UF2   0x57u
#define SCB_AIRCR       REG(0xE000ED0CUL)
#define AIRCR_SYSRESET  0x05FA0004UL
