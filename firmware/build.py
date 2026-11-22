#!/usr/bin/env python3
# Copyright 2026, FreeTouch
"""Build the FreeTouch dongle firmware: build/freetouch-dongle.uf2 (and .elf/.bin/.map).

Needs the Arm GNU toolchain (arm-none-eabi-gcc). It is looked for in FREETOUCH_ARM_GCC (its bin
directory), then on PATH. Never flashes anything; see docs/INSTALL.md for that.

    python build.py [--exposure-period US]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
THIRD = Path(os.environ.get("FREETOUCH_THIRD_PARTY", HERE / "third_party"))
OUT = HERE / "build"
TINYUSB = THIRD / "tinyusb"
NRFX = THIRD / "nrfx"
CMSIS = THIRD / "cmsis-core" / "CMSIS" / "Core" / "Include"
EXE = ".exe" if os.name == "nt" else ""
# The nice!nano's Adafruit UF2 bootloader runs apps from here; family nRF52840.
APP_BASE = 0x26000


def tool(name):
    bin_dir = os.environ.get("FREETOUCH_ARM_GCC")
    if bin_dir:
        path = Path(bin_dir) / (name + EXE)
        if path.is_file():
            return str(path)
        sys.exit(f"{path} not found (FREETOUCH_ARM_GCC={bin_dir})")
    found = shutil.which(name)
    if not found:
        sys.exit(f"{name} not found: install the Arm GNU toolchain and put its bin directory on PATH, "
                 "or set FREETOUCH_ARM_GCC to it")
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exposure-period", type=int, default=19200,
                    help="camera period the polls announce, us (19200 is the Rift's)")
    ap.add_argument("--name", default="freetouch-dongle")
    args = ap.parse_args()

    OUT.mkdir(exist_ok=True)
    elf, raw, uf2 = (OUT / (args.name + s) for s in (".elf", ".bin", ".uf2"))
    includes = [SRC, TINYUSB / "src", NRFX, NRFX / "mdk", NRFX / "hal", NRFX / "drivers" / "include",
                NRFX / "drivers" / "src", CMSIS]
    sources = [SRC / "startup_usb.c", SRC / "dongle.c", SRC / "usb_descriptors.c",
               TINYUSB / "src/tusb.c", TINYUSB / "src/common/tusb_fifo.c",
               TINYUSB / "src/device/usbd.c", TINYUSB / "src/device/usbd_control.c",
               TINYUSB / "src/class/cdc/cdc_device.c",
               TINYUSB / "src/portable/nordic/nrf5x/dcd_nrf5x.c"]
    cmd = [tool("arm-none-eabi-gcc"), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-O2", "-g",
           "-ffreestanding", "-nostdlib", "-fno-builtin", "-Wall", "-Wextra", "-Werror", "-fno-common",
           "-ffunction-sections", "-fdata-sections", "-DNRF52840_XXAA", "-DCFG_TUSB_MCU=OPT_MCU_NRF5X",
           f"-DDM_EXPOSURE_PERIOD_US={args.exposure_period}u"]
    for inc in includes:
        cmd += ["-I", str(inc)]
    cmd += ["-T", str(HERE / "link.ld"), "-Wl,--gc-sections", f"-Wl,-Map,{OUT / (args.name + '.map')}",
            "-Wl,--entry=reset_handler", "-o", str(elf)] + [str(s) for s in sources]
    subprocess.run(cmd, check=True)
    subprocess.run([tool("arm-none-eabi-objcopy"), "-O", "binary", str(elf), str(raw)], check=True)
    subprocess.run([sys.executable, str(HERE / "mkuf2.py"), str(raw), str(uf2), hex(APP_BASE)], check=True)
    subprocess.run([tool("arm-none-eabi-size"), str(elf)], check=True)
    digest = hashlib.sha256(uf2.read_bytes()).hexdigest()
    (OUT / "manifest.json").write_text(json.dumps(
        {"uf2": uf2.name, "sha256": digest, "bytes": uf2.stat().st_size}, indent=2) + "\n")
    print(f"{uf2}  sha256 {digest}")


if __name__ == "__main__":
    main()
