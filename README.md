# FreeTouch dongle

Use Oculus Rift CV1 Touch controllers without a Rift headset.

The headset is normally what talks to the Touch controllers by radio and what tells the CV1
sensors when to expose. The FreeTouch dongle does both jobs from a small nRF52840 board (a
nice!nano) plugged into USB:

- it pairs CV1 Touch controllers and polls them every 2 ms, as the headset's radio does;
- it hands their reports to the PC over a USB serial port;
- its polls carry the camera exposure timing, so CV1 sensors synced to it expose in step with the
  controllers' IR LED flashes, exactly as with a headset.

This repository is the dongle's firmware and a small C library, `libfreetouch`, that any CV1 app
can drop in to support it: find the port, read the records, decrypt the controller reports, send
commands. There is no configuration: each dongle makes its own radio address and encryption key
on first boot and reports them to the host.

## What's here

| Path | What |
|---|---|
| `firmware/` | The dongle firmware (C, bare metal on the nRF52840 with TinyUSB). `build.py` builds a `.uf2`. |
| `host/` | `libfreetouch`: `include/freetouch/freetouch.h` and its sources. Plain C99, Windows and Linux. |
| `tools/freetouch-cli.c` | A command line tool: dongle info, live controller monitor, pairing, haptics, sleep, firmware update. Also a worked example of the library. |
| `tests/` | The library's tests. |
| `docs/` | [Install](docs/INSTALL.md), [pairing](docs/PAIRING.md), [protocol](docs/PROTOCOL.md) and [integrating](docs/INTEGRATING.md). |

## Quick start

1. Get a nice!nano (v2, nRF52840) or a board with the same Adafruit UF2 bootloader.
2. Install the firmware: double-tap its reset, copy `freetouch-dongle.uf2` from the latest
   release onto the `NICENANO` drive that appears. See [docs/INSTALL.md](docs/INSTALL.md).
3. Pair your controllers: `freetouch-cli pair`, then hold Menu+Y (left) or Oculus+B (right).
   See [docs/PAIRING.md](docs/PAIRING.md).
4. Check them: `freetouch-cli monitor` shows each controller's report rate, signal and inputs.
5. Use an app that supports the dongle, or add support to yours with
   [docs/INTEGRATING.md](docs/INTEGRATING.md).

## Building

Host side (library, CLI, tests), any C99 compiler with CMake:

```
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Firmware, with the Arm GNU toolchain (`arm-none-eabi-gcc`) on PATH or in `FREETOUCH_ARM_GCC`:

```
python firmware/build.py
```

The firmware's third-party sources (TinyUSB, nrfx, CMSIS) are vendored in
`firmware/third_party/`, so the build needs nothing else. The same sources build the same
`.uf2`, byte for byte.

## Limits

- CV1 Touch only (on their standard firmware). Not the Oculus Remote, Quest controllers or Rift S
  Touch.
- Up to four controllers: two per hand. Two controllers of one hand share a radio session, so a
  haptic command buzzes both.
- The radio shares 2.4 GHz with Wi-Fi and Bluetooth. Plug the dongle into a USB 2.0 port or a short
  extension rather than next to a USB 3 port or hub, whose noise costs replies.
- Pairing a controller to the dongle replaces its pairing with a headset. Pair it with the Oculus
  app again to use it with a Rift.

## Licence

MIT for everything in this repository except the vendored third-party sources, which keep their
own licences. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).

This is an independent implementation for interoperability with hardware its users own. It is not
affiliated with or endorsed by Meta or Oculus, and it contains no code or firmware of theirs.
