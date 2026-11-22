# Installing the firmware

## What you need

- A **nice!nano v2** (nRF52840) or another nRF52840 board with the **Adafruit nRF52 UF2
  bootloader** and its app at `0x26000`. The firmware is built and tested on the nice!nano; other
  boards with the same bootloader should work but are untested.
- A USB data cable.
- `freetouch-dongle.uf2` from the latest release, or your own build (`python firmware/build.py`
  writes `firmware/build/freetouch-dongle.uf2`).

No soldering, programmer or drivers: the board installs firmware by drag and drop, and Windows 10+,
Linux and macOS all have USB serial (CDC) drivers built in.

## Install

1. Plug the board in.
2. Put it in its bootloader:
   - a board fresh from the shop, or running other firmware: **double-tap its reset button**
     (on the nice!nano, short `RST` to `GND` twice quickly);
   - a board already running FreeTouch: `freetouch-cli bootloader`. Any program that opens the port
     at 1200 baud does the same.
3. A USB drive named **`NICENANO`** appears (on other boards it may have another name).
4. Copy `freetouch-dongle.uf2` onto it. The board flashes itself, the drive disappears and the board
   restarts as a USB serial port: **USB ID `CAFE:4001`**, a COM port on Windows, `/dev/ttyACM*` on
   Linux.
5. Check it: `freetouch-cli info` shows the firmware version and the dongle's radio address.

On its first start the firmware makes the dongle's own radio address and encryption key from the
chip's random number generator and stores them in flash.

## Updating

Repeat the install with the new `.uf2`. Firmware updates **keep** the dongle's address and key, and
with them every controller paired to it: the identity lives in a flash page (`0xF3000`, the
bootloader's user-data area) that UF2 updates never write.

Erasing the whole chip, or updating the bootloader itself, can clear that page. The dongle then makes
a new identity on its next start, and every controller has to be paired again.

## Placement

The dongle and the controllers talk on 2.4 GHz, beside Wi-Fi and Bluetooth. For the fewest lost
reports:

- use a USB 2.0 port, or a short USB extension, rather than a port next to a USB 3 device or hub:
  USB 3 radiates noise across 2.4 GHz;
- keep it in the open, roughly where a headset would have been, not behind the PC case.

`freetouch-cli monitor` shows each controller's report rate. The dongle polls 500 times a second;
about 400 to 500 reports a second is a healthy link.

## Going back

To use the board for something else, put it in its bootloader (step 2) and copy that firmware's
`.uf2` instead. The bootloader itself is never touched by FreeTouch.
