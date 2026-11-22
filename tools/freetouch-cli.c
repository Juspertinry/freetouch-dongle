// Copyright 2026, FreeTouch
/*!
 * @file
 * @brief  freetouch-cli: talk to a FreeTouch dongle from a terminal, and an example of libfreetouch.
 */

#include "freetouch/freetouch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t
now_ms(void)
{
	return GetTickCount64();
}
#else
#include <time.h>
static uint64_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

static const char *usage =
    "usage: freetouch-cli [--port PORT] COMMAND\n"
    "  info                   firmware, chip and link address of the dongle\n"
    "  monitor [SECONDS]      each controller's report rate, signal and inputs, once a second\n"
    "  pair [OFFSET_US]       pair the next CV1 Touch in pairing mode (left: Menu+Y, right: Oculus+B)\n"
    "  haptic left|right [AMPLITUDE 0-255] [MS]\n"
    "  sleep                  power the controllers off (a button wakes each)\n"
    "  hop auto|0-36          let the dongle find the hop increment (the default), or fix it (0 parks)\n"
    "  bootloader             restart the dongle into its UF2 bootloader, for a firmware update\n"
    "The port is found by its USB ID unless --port names it (COM5, /dev/ttyACM0).\n";

struct dongle
{
	freetouch_port *port;
	struct freetouch_parser parser;
	bool have_identity, have_firmware, have_hop;
	struct freetouch_identity identity;
	struct freetouch_hop hop;
	uint32_t firmware, chip, period_us;
};

static bool
send_cmd(struct dongle *d, const uint8_t *cmd, size_t len)
{
	return freetouch_port_write(d->port, cmd, len) == (int)len;
}

/*!
 * Reads for up to @p ms and hands each record to @p on (if any); the identity and firmware are noted
 * on the way. Returns false when the port failed or @p on asked to stop.
 */
typedef bool (*record_fn)(struct dongle *d,
                          enum freetouch_record kind,
                          const struct freetouch_input *in,
                          const struct freetouch_status *st,
                          void *user);

static bool
pump(struct dongle *d, uint64_t ms, record_fn on, void *user)
{
	const uint64_t end = now_ms() + ms;
	while (now_ms() < end) {
		size_t room;
		uint8_t *space = freetouch_parser_space(&d->parser, &room);
		int got = freetouch_port_read(d->port, space, room, 50);
		if (got < 0) {
			fprintf(stderr, "freetouch-cli: the dongle stopped answering (unplugged?)\n");
			return false;
		}
		freetouch_parser_filled(&d->parser, (size_t)got);
		struct freetouch_input in;
		struct freetouch_status st;
		struct freetouch_reply reply;
		enum freetouch_record kind;
		while ((kind = freetouch_parser_next(&d->parser, &in, &st, &reply)) != FREETOUCH_RECORD_NONE) {
			if (kind == FREETOUCH_RECORD_STATUS) {
				if (freetouch_status_identity(&st, &d->identity)) {
					d->have_identity = true;
				}
				if (freetouch_status_hop(&st, &d->hop)) {
					d->have_hop = true;
				}
				// code 5 with a = 34 is a read request's echo, not the firmware
				if (st.code == FREETOUCH_STATUS_FIRMWARE && st.a != 34) {
					d->firmware = st.a & 0xffffu;
					d->period_us = (st.a >> 16) * 100u;
					d->chip = st.b;
					d->have_firmware = true;
				}
			}
			if (on != NULL && !on(d, kind, &in, &st, user)) {
				return false;
			}
		}
	}
	return true;
}

static bool
open_dongle(struct dongle *d, const char *path)
{
	char found[64];
	if (path == NULL) {
		if (!freetouch_port_find(found, sizeof(found))) {
			fprintf(stderr, "freetouch-cli: no FreeTouch dongle found (USB %04X:%04X)\n", FREETOUCH_VID,
			        FREETOUCH_PID);
			return false;
		}
		path = found;
	}
	d->port = freetouch_port_open(path);
	if (d->port == NULL) {
		fprintf(stderr, "freetouch-cli: cannot open %s (another program has it?)\n", path);
		return false;
	}
	freetouch_parser_init(&d->parser);
	// the identity comes when a host opens the port; ask too, for firmware that waits
	uint8_t cmd[1];
	send_cmd(d, cmd, freetouch_cmd_identity(cmd));
	return true;
}

/*
 * info
 */

static bool
info_done(struct dongle *d,
          enum freetouch_record kind,
          const struct freetouch_input *in,
          const struct freetouch_status *st,
          void *user)
{
	(void)kind, (void)in, (void)st, (void)user;
	return !(d->have_identity && d->have_firmware && d->have_hop);
}

static int
cmd_info(struct dongle *d)
{
	// the firmware status comes every 5 s
	pump(d, 6000, info_done, NULL);
	if (d->have_firmware) {
		printf("firmware  %u.%u (0x%04X), exposure period %u us\n", d->firmware >> 8, d->firmware & 0xffu,
		       d->firmware, d->period_us);
		printf("chip      %08X\n", d->chip);
	} else {
		printf("firmware  (no status in 6 s)\n");
	}
	if (d->have_identity) {
		printf("address   %02x%02x%02x%02x%02x\n", d->identity.address[0],
		       d->identity.address[1], d->identity.address[2], d->identity.address[3], d->identity.address[4]);
		printf("link key  present (the dongle made it; the host reads it at every start)\n");
	} else {
		printf("identity  none: firmware before 0x0300 uses the old shared lab key\n");
	}
	if (d->have_hop) {
		if (d->hop.sweeping) {
			printf("hopping   looking for the increment (needs a controller awake)\n");
		} else if (d->hop.increment == 0) {
			printf("hopping   parked on one channel%s\n", d->hop.automatic ? " (no controller heard yet)" : "");
		} else {
			printf("hopping   increment %u%s\n", d->hop.increment,
			       d->hop.automatic ? " (found by the dongle)" : " (fixed by the host)");
		}
	}
	return d->have_firmware ? 0 : 1;
}

/*
 * monitor
 */

struct controller
{
	uint8_t session, slot;
	unsigned reports;
	uint8_t rssi;
	struct freetouch_touch touch;
};

struct monitor
{
	struct controller c[8];
	unsigned n, bad_mic, no_key;
};

static bool
monitor_record(struct dongle *d,
               enum freetouch_record kind,
               const struct freetouch_input *in,
               const struct freetouch_status *st,
               void *user)
{
	(void)d, (void)st;
	struct monitor *m = user;
	if (kind == FREETOUCH_RECORD_BAD_MIC) {
		m->bad_mic++;
	}
	if (kind == FREETOUCH_RECORD_NO_KEY) {
		m->no_key++;
	}
	if (kind != FREETOUCH_RECORD_INPUT) {
		return true;
	}
	struct controller *c = NULL;
	for (unsigned i = 0; i < m->n; i++) {
		if (m->c[i].session == in->session && m->c[i].slot == in->slot) {
			c = &m->c[i];
		}
	}
	if (c == NULL && m->n < sizeof(m->c) / sizeof(m->c[0])) {
		c = &m->c[m->n++];
		memset(c, 0, sizeof(*c));
		c->session = in->session;
		c->slot = in->slot;
	}
	if (c != NULL) {
		c->reports++;
		c->rssi = in->rssi;
		freetouch_touch_decode(in->report, &c->touch);
	}
	return true;
}

static int
cmd_monitor(struct dongle *d, unsigned seconds)
{
	struct monitor m;
	memset(&m, 0, sizeof(m));
	for (unsigned s = 0; seconds == 0 || s < seconds; s++) {
		for (unsigned i = 0; i < m.n; i++) {
			m.c[i].reports = 0;
		}
		m.bad_mic = m.no_key = 0;
		if (!pump(d, 1000, monitor_record, &m)) {
			return 1;
		}
		printf("--- %u s\n", s + 1);
		if (m.no_key > 0) {
			printf("%u reports before the dongle's identity came\n", m.no_key);
		}
		for (unsigned i = 0; i < m.n; i++) {
			const struct controller *c = &m.c[i];
			const struct freetouch_touch *t = &c->touch;
			printf("%-5s slot %u  %3u reports/s  -%u dBm  buttons %c%c%c%c  trigger %4u  grip %4u  stick %4u %4u\n",
			       c->session == FREETOUCH_SESSION_LEFT ? "left" : "right", c->slot, c->reports, c->rssi,
			       t->buttons & FREETOUCH_BUTTON_A_X ? (c->session == FREETOUCH_SESSION_LEFT ? 'X' : 'A') : '.',
			       t->buttons & FREETOUCH_BUTTON_B_Y ? (c->session == FREETOUCH_SESSION_LEFT ? 'Y' : 'B') : '.',
			       t->buttons & FREETOUCH_BUTTON_SYSTEM ? 'S' : '.', t->buttons & FREETOUCH_BUTTON_STICK ? 'T' : '.',
			       t->trigger, t->grip, t->stick[0], t->stick[1]);
		}
		if (m.bad_mic > 0) {
			printf("%u reports did not decrypt (a controller paired to another dongle or headset?)\n",
			       m.bad_mic);
		}
		fflush(stdout);
	}
	return 0;
}

/*
 * pair
 */

static bool
pair_record(struct dongle *d,
            enum freetouch_record kind,
            const struct freetouch_input *in,
            const struct freetouch_status *st,
            void *user)
{
	(void)d, (void)in;
	int *state = user;
	if (kind == FREETOUCH_RECORD_STATUS && st->code == FREETOUCH_STATUS_PAIRING) {
		*state = (int)st->a;
		return st->a == FREETOUCH_PAIRING_STARTED;
	}
	return true;
}

static int
cmd_pair(struct dongle *d, unsigned offset_us)
{
	uint8_t cmd[4];
	if (!send_cmd(d, cmd, freetouch_cmd_pair(cmd, (uint16_t)offset_us))) {
		return 1;
	}
	printf("pairing: put a CV1 Touch in pairing mode now (left: hold Menu+Y, right: hold Oculus+B)\n");
	int state = 0;
	pump(d, 65000, pair_record, &state);
	switch (state) {
	case FREETOUCH_PAIRING_PAIRED: printf("paired\n"); return 0;
	case FREETOUCH_PAIRING_FAILED: printf("pairing failed: try again, closer to the dongle\n"); return 1;
	case FREETOUCH_PAIRING_TIMED_OUT: printf("no controller in pairing mode within 60 s\n"); return 1;
	default: printf("no pairing result from the dongle (firmware too old?)\n"); return 1;
	}
}

int
main(int argc, char **argv)
{
	const char *port = NULL;
	int i = 1;
	if (i + 1 < argc && strcmp(argv[i], "--port") == 0) {
		port = argv[i + 1];
		i += 2;
	}
	if (i >= argc) {
		fputs(usage, stderr);
		return 2;
	}
	const char *cmd = argv[i++];
	struct dongle d;
	memset(&d, 0, sizeof(d));
	if (!open_dongle(&d, port)) {
		return 1;
	}
	int rc = 2;
	uint8_t buf[11];
	if (strcmp(cmd, "info") == 0) {
		rc = cmd_info(&d);
	} else if (strcmp(cmd, "monitor") == 0) {
		rc = cmd_monitor(&d, i < argc ? (unsigned)atoi(argv[i]) : 0);
	} else if (strcmp(cmd, "pair") == 0) {
		rc = cmd_pair(&d, i < argc ? (unsigned)atoi(argv[i]) : 0);
	} else if (strcmp(cmd, "haptic") == 0 && i < argc) {
		const uint8_t session =
		    strcmp(argv[i], "left") == 0 ? FREETOUCH_SESSION_LEFT : FREETOUCH_SESSION_RIGHT;
		const int amp = i + 1 < argc ? atoi(argv[i + 1]) : 160;
		const int ms = i + 2 < argc ? atoi(argv[i + 2]) : 300;
		send_cmd(&d, buf, freetouch_cmd_haptic(buf, session, (uint8_t)(amp < 0 ? 0 : amp > 255 ? 255 : amp), 320));
		pump(&d, (uint64_t)(ms < 0 ? 0 : ms), NULL, NULL);
		rc = send_cmd(&d, buf, freetouch_cmd_haptic(buf, session, 0, 320)) ? 0 : 1;
	} else if (strcmp(cmd, "sleep") == 0) {
		rc = send_cmd(&d, buf, freetouch_cmd_sleep(buf)) ? 0 : 1;
		// the controllers act on it over the next couple of seconds of polls
		pump(&d, 2500, NULL, NULL);
	} else if (strcmp(cmd, "hop") == 0 && i < argc) {
		const int inc = strcmp(argv[i], "auto") == 0 ? FREETOUCH_HOP_AUTO : atoi(argv[i]);
		if (inc == FREETOUCH_HOP_AUTO || (inc >= 0 && inc <= 36)) {
			rc = send_cmd(&d, buf, freetouch_cmd_hop(buf, (uint8_t)inc)) ? 0 : 1;
		} else {
			fputs(usage, stderr);
		}
	} else if (strcmp(cmd, "bootloader") == 0) {
		rc = send_cmd(&d, buf, freetouch_cmd_bootloader(buf)) ? 0 : 1;
		if (rc == 0) {
			printf("the dongle restarts as a USB drive (NICENANO): copy the .uf2 onto it\n");
		}
	} else {
		fputs(usage, stderr);
	}
	freetouch_port_close(d.port);
	return rc;
}
