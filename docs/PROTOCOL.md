# The dongle's USB protocol

The dongle is a USB CDC serial port, **USB ID `CAFE:4001`**. Baud rate, parity and flow control are
ignored, with one exception: opening the port at **1200 baud** restarts the dongle into its UF2
bootloader. Open it at anything else (115200 is customary) with DTR raised; the dongle streams only
while a host holds the port open.

Everything here is implemented in `host/src/stream.c` (`libfreetouch`), with tests.

## Records: dongle to host

The dongle sends fixed **54-byte records**, each starting with a 4-byte magic, so a reader that
loses bytes finds its place again at the next magic. All numbers are little-endian.

### `CV1I`: a controller report

| Bytes | Field |
|---|---|
| 0-3 | `CV1I` |
| 4-7 | sequence number, +1 per record; a gap means records were lost |
| 8-11 | `poll_us`: the dongle's clock (us) at the poll this report answers |
| 12 | radio CRC check (1 ok) |
| 13 | radio address the reply came on (1) |
| 14 | radio channel |
| 15 | signal strength, -dBm |
| 16 | uplink length (34) |
| 17 | high nibble: the reply slot (1..4, as paired); low nibble: the session |
| 18-19 | records the dongle could not queue over USB, so far |
| 20-53 | the controller's uplink, 34 bytes: a 5-byte header (byte 2 the session, byte 4 flags), the 25-byte report **encrypted**, a 4-byte MIC |

The **session** is the controller's hand: **2 left, 3 right**. Two controllers of one hand share a
session and differ by reply slot.

### `CV1S`: status

| Bytes | Field |
|---|---|
| 0-3 | `CV1S` |
| 4-7 | code |
| 8-11 | a |
| 12-15 | b |
| 16-19 | records the dongle could not queue, so far |
| 20-53 | raw bytes, for the codes that carry them |

| Code | When | a | b / raw |
|---|---|---|---|
| 1 | after boot, and after the radio recovered from a fault (0x0302+) | 1 when a paired controller answered (0 after a fault) | fault code, 0 none |
| 2 | every 5 s | polls sent | replies (low 16 bits), overruns << 16 |
| 3 | every 5 s | 0x04 \| IR-config acks << 8 | 0x0f \| acks << 8 |
| 4 | every 5 s, per reply slot heard | slot << 24 \| replies | when the replies ended, us after the poll: min \| max << 12 \| (average / 16) << 24 |
| 5 | every 5 s | firmware revision (0x0301 = 3.1) \| exposure period / 100 us << 16 | the chip's ID (FICR DEVICEID0) |
| 5 | after an `R` read | 34 | raw: the request frame as sent |
| 6 | pairing | 1 started, 2 paired, 3 failed, 4 timed out | |
| 7 | every 10 ms | a poll's timestamp on the `poll_us` clock | the exposure period, us |
| 8 | every 5 s, and when a hop sweep ends | channel map in use, bits 0-31 | bits 0-4: map bits 32-36; 8-13: hop increment in use (0 parked); 14: auto hopping; 15: sweep running (0x0301) |
| 9 | when a host opens the port, on `I`, every 5 s | 21 (raw length) | b: firmware revision (0x0301 on; 0 before). Raw: radio address (5 bytes) then the link key (16 bytes) |

Code 9, the **identity**, is new in firmware 0x0300. Each dongle makes its own address and key on
first boot and keeps them across updates. Earlier firmware used one fixed address and a shared lab
key and sends no code 9.

### Hopping

The link hops over the channel map. Each poll carries the map and the current hop index. A controller
steps the index by its own increment, which it never reports. Every controller paired to one dongle
identity follows the same increment, and it changes with the identity.

From 0x0301 the dongle finds the increment itself (auto, the default). It waits for a reply, steps by
one candidate, and keeps the candidate if the next polls are answered; a full sweep takes about 3 s.
The result is stored with the identity, so later boots hop at once. If the replies later drop to the
one in ~37 a wrong increment gives, it sweeps again. Hosts should not send `H` unless they mean to fix
the increment. Firmware 0x0300 parks on one channel unless told an increment.

### `CV1R`: a command reply

| Bytes | Field |
|---|---|
| 0-3 | `CV1R` |
| 4 | length (up to 48) |
| 5-52 | the reply frame, raw |
| 53 | the reply slot it came in |

Replies answer the `R` read command (below): a config block read or a device info request.

## Decrypting a report

The link is **AES-128-CCM** (RFC 3610) with the dongle's key (identity, code 9):

- nonce, 13 bytes: `00 00 00 00 00 5c cf`, then 6 bytes little-endian of
  `slot_us | b11 << 32`, where `slot_us` is the poll slot the report answers and `b11` is 0
  (firmware before 0x020B: 1);
- associated data: one zero byte;
- ciphertext: uplink bytes 5-29 (25 bytes); MIC: uplink bytes 30-33 (4 bytes).

A controller may answer a poll up to five 2 ms slots after it was sent, so try `slot_us` =
`poll_us`, `poll_us + 2000`, ... `poll_us + 10000`; the one whose MIC checks is the right one.
`freetouch_decrypt` does exactly this.

## The report

The 25 decrypted bytes are the Rift headset's radio message for a Touch, less its first three bytes
(flags and device type):

| Bytes | Field |
|---|---|
| 0-3 | the controller's clock, us |
| 4-9 | accelerometer x, y, z: int16, raw counts |
| 10-15 | gyro x, y, z: int16, raw counts |
| 16 | buttons: 0x01 A/X, 0x02 B/Y, 0x04 Oculus/Menu, 0x08 stick click |
| 17-21 | trigger, grip, stick x, stick y: four 10-bit values packed low bit first |
| 22 | a multiplexed reading's channel (capacitive sensors and more, one per report) |
| 23-24 | its value |

The IMU counts are scaled by each controller's factory calibration, which it returns to config
block reads (`R`). OpenHMD's Rift driver documents the calibration and the capacitive channels.

## Commands: host to dongle

Single bytes, some followed by arguments, written to the port as they are. Unknown bytes are
ignored.

| Command | Arguments | Effect |
|---|---|---|
| `I` | | send the identity (code 9) now |
| `P` | | pair the next controller in pairing mode, within 60 s; status code 6 reports the result |
| `U` | LE16 us | the reply delay the next `P` writes to the controller, 400-1620 us |
| `Q` | byte 0-4 | the next `P`'s reply slot by number: 1-3 = 1000/1250/1500 us, 4 = 750 us, 0 = by hand |
| `V` | session, amplitude, LE16 frequency | vibrate that session's controllers, amplitude 0-255 until 0 is sent (or, from 0x0302, the host closes the port); frequency >= 320 buzzes at 320 Hz, below at 160 Hz |
| `Z` | byte (ignored) | power the controllers off; a button wakes each |
| `A` | byte | 1: keep the controllers from dozing (refreshed every 30 min; the default); 0: let them doze and power off on their own |
| `L` | LE32 us | the controllers' IR LED pulse width, 2-15000 us (default 35) |
| `X` | LE32 us | the camera exposure period the polls announce, 5000-65535 us (default 19200, the headset's) |
| `E` | LE32 signed us | slew the polls' exposure phase by this much, gradually (+-33333) |
| `M` | 5 bytes | the radio channel map (37 bits) the link hops within |
| `H` | byte | 0-36: fix the hop increment (0 parks on one channel); 255: back to auto (0x0301, the default) |
| `R` | LE32 | one read command on the next poll, with this argument: for config block reads, offset \| length << 16 |
| `O` | byte | the opcode `R` sends (0x0a config block read, the default; 0x88 device info) |
| `S` | byte | the session `R` goes to (0: the controller-reported one) |
| `Y` | byte | poll byte 11 for `R` (0xff keeps the poll's own) |
| `K` | byte | poll layout flags (advanced; the default 3 matches a headset) |
| `b`, `B` | | restart into the UF2 bootloader |

`libfreetouch` builds the common ones (`freetouch_cmd_*`).

## Timing

- The dongle polls every **2 ms**. Each controller answers at its reply delay after the poll.
- Every poll carries the **exposure period** (19200 us by default) and a timestamp. CV1 sensors set
  to the dongle's radio address time their exposures off the polls, and the controllers time their
  IR LED flashes off the same polls, so the two line up as with a headset.
- Status code 7, every 10 ms, gives a poll timestamp on the `poll_us` clock even with no controller
  connected, so a host can map the dongle's clock to its own for the sensors' exposures.
