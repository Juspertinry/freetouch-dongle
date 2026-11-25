# Adding FreeTouch dongle support to a CV1 app

An app that already supports CV1 Touch through the Rift headset (OpenHMD, Monado, or its own code)
gets controller input from the headset's radio reports and syncs the CV1 sensors to the headset's
radio address. With the dongle, both come from `libfreetouch` instead:

1. **Controller input**: the dongle's records, decrypted, give the same Touch report the headset's
   radio interface does.
2. **Sensor sync**: the dongle's radio address takes the place of the headset's.
3. **Exposure timing**: the dongle's poll clock takes the place of the headset's timestamps.

Copy `host/` into your tree (or build it with the CMake file at the root) and include
`freetouch/freetouch.h`. The library is C99 with no dependencies beyond the OS (SetupAPI on Windows),
allocates nothing outside `freetouch_port_open`, and uses no threads: run it from your reader thread.

## Reading the dongle

```c
#include <freetouch/freetouch.h>

char path[64];
if (!freetouch_port_find(path, sizeof path))
	return; /* no dongle */
freetouch_port *port = freetouch_port_open(path);

struct freetouch_parser parser;
freetouch_parser_init(&parser);

/* the identity comes when the port opens; asking costs nothing */
uint8_t cmd[1];
freetouch_port_write(port, cmd, freetouch_cmd_identity(cmd));

for (;;) {
	size_t room;
	uint8_t *space = freetouch_parser_space(&parser, &room);
	int got = freetouch_port_read(port, space, room, 50);
	if (got < 0)
		break; /* unplugged: close, then look for it again */
	freetouch_parser_filled(&parser, (size_t)got);

	struct freetouch_input in;
	struct freetouch_status st;
	enum freetouch_record kind;
	while ((kind = freetouch_parser_next(&parser, &in, &st, NULL)) != FREETOUCH_RECORD_NONE) {
		struct freetouch_identity id;
		if (kind == FREETOUCH_RECORD_STATUS && freetouch_status_identity(&st, &id)) {
			/* the parser now decrypts with id.key; hand id.address to the sensors (below) */
		}
		if (kind == FREETOUCH_RECORD_INPUT) {
			struct freetouch_touch t;
			freetouch_touch_decode(in.report, &t);
			/* in.session: 2 left, 3 right; in.slot tells two of one hand apart */
		}
	}
}
```

Records that arrive before the identity come back as `FREETOUCH_RECORD_NO_KEY`. That lasts a few
milliseconds after the port opens (and up to 5 s for a host that opened it before the dongle
finished starting); drop them.

## If your code already speaks the headset's radio report

The decrypted 25 bytes are the headset radio's Touch message less its flags and device type. To
reuse an existing parser (OpenHMD's `decode_rift_radio_report`, for one), rebuild the 64-byte radio
report around it:

```c
unsigned char report[64] = {0};
report[0] = 0x0c;                 /* radio report id */
report[3] = 0x1c;                 /* message flags: a Touch */
report[5] = in.session | (unit << 4); /* device type; unit 0 for the first controller of a hand */
memcpy(report + 6, in.report, FREETOUCH_REPORT_SIZE);
```

`unit` distinguishes a second controller of the same hand: map each (session, slot) pair you see to
0 for the first, 1 for the second.

## Syncing the CV1 sensors

A CV1 sensor exposes in step with whatever radio it is told to listen to. With a headset, apps read
the headset's radio address and pass it to the sensors. Pass the dongle's `id.address` in its place,
in the same byte order: for OpenHMD, it is the `radio_address` given to `rift_tracker_new`.

The sensors then time their exposures off the dongle's polls, and the controllers flash their IR
LEDs off the same polls, so the LEDs land inside the exposures as with a headset.

## Exposure times

Pose fusion needs each camera frame's exposure on the controllers' clock. With a headset, frames are
stamped from the headset's IMU timestamps; with the dongle:

- each input record's `poll_us` is the dongle's clock (us) at the poll it answered;
- status code 7 (every 10 ms) carries a poll timestamp even with no controller connected;
- exposures fall on the exposure period (19200 us by default) of the dongle's clock.

Map the dongle's clock to your host clock with a lower-envelope fit of (`poll_us`, arrival time):
the fastest records of each second bound the mapping from above.

`poll_us` is 32 bits and wraps every 71.6 minutes. That is the CV1 link's own width: every poll
carries it, the headset sends the same field, and the controllers build their nonce from it, so it
cannot be widened. Extend it on the host: take each new value's step from the last as a signed
32-bit difference and add that to a 64-bit clock. Records arrive a slot or two out of order, so a
small negative step is a late record, not a wrap.

## Haptics, pairing and sleep

Build the commands with `freetouch_cmd_haptic`, `freetouch_cmd_pair`, `freetouch_cmd_sleep` and the
others, and write them to the port. Pairing results arrive as status code 6
(`enum freetouch_pairing`). See [PROTOCOL.md](PROTOCOL.md) for the full command set.

## Older firmware

Firmware before 0x0300 sends no identity: it used one fixed radio address and a shared lab key. The
library has no built-in key, so such a dongle's reports stay `FREETOUCH_RECORD_NO_KEY`. Update the
dongle (docs/INSTALL.md) and pair its controllers again.
