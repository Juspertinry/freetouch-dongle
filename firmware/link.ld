/* Copyright 2026, FreeTouch */
/* App slot of the nice!nano's Adafruit UF2 bootloader, fixed between S140 (ends 0x26000) and the bootloader at 0xF4000.
 * RAM starts above 0 so the MBR keeps its area at the bottom. */
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00026000, LENGTH = 0x0002A000
  RAM   (rwx) : ORIGIN = 0x20002000, LENGTH = 0x00010000
}

ENTRY(reset_handler)

SECTIONS
{
  .text : {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _etext = .;

  .data : AT (_etext) {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM

  .bss (NOLOAD) : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
