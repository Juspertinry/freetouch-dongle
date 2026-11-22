# Copyright 2026, FreeTouch
"""Pack a raw .bin into a UF2 for the nice!nano's Adafruit bootloader.

Family 0xADA52840 and a 0x26000 load address are both fixed by that bootloader:
S140 occupies 0x1000-0x26000 and the bootloader itself lives at 0xF4000, so the
app slot is the only place this can go. Getting either wrong produces a file the
bootloader silently ignores, which looks exactly like a dead board.
"""
import struct, sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x2000
FAMILY = 0xADA52840
PAYLOAD = 256


def convert(src, dst, base):
    data = open(src, 'rb').read()
    if len(data) % PAYLOAD:
        data += b'\x00' * (PAYLOAD - len(data) % PAYLOAD)
    nblocks = len(data) // PAYLOAD
    out = bytearray()
    for i in range(nblocks):
        chunk = data[i * PAYLOAD:(i + 1) * PAYLOAD]
        blk = struct.pack('<8I', MAGIC0, MAGIC1, FLAG_FAMILY,
                          base + i * PAYLOAD, PAYLOAD, i, nblocks, FAMILY)
        blk += chunk + b'\x00' * (476 - PAYLOAD)
        blk += struct.pack('<I', MAGIC_END)
        assert len(blk) == 512
        out += blk
    open(dst, 'wb').write(out)
    print('%s -> %s: %d bytes, %d blocks, 0x%06x-0x%06x'
          % (src, dst, len(data), nblocks, base, base + len(data)))


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2], int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x26000)
