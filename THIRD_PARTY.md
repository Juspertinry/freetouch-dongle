# Third-party code

The firmware is built with the subsets of these projects vendored in `firmware/third_party/`.
Each directory keeps its project's own licence file, and the built firmware includes code from
all three.

| Directory | Project | Licence |
|---|---|---|
| `firmware/third_party/tinyusb` | [TinyUSB](https://github.com/hathach/tinyusb), the USB device stack | MIT (`tinyusb/LICENSE`) |
| `firmware/third_party/nrfx` | [nrfx](https://github.com/NordicSemiconductor/nrfx), Nordic's nRF52 peripheral drivers (USBD, power) | BSD-3-Clause (`nrfx/LICENSE`) |
| `firmware/third_party/cmsis-core` | [CMSIS](https://github.com/ARM-software/CMSIS_5) Core headers | Apache-2.0 (`cmsis-core/LICENSE.txt`) |

`firmware/src/nrfx_glue.h` is adapted from Nordic's nrfx template and keeps Nordic's BSD-3-Clause
notice.

The host library and the CLI use only the C standard library and the operating system's own
APIs (SetupAPI on Windows).
