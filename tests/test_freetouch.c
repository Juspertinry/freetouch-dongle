// Copyright 2026, FreeTouch
/*!
 * @file
 * @brief  libfreetouch's stream: the link's AES-CCM against an independent implementation, the
 *         parser end to end (identity, then an input decrypted with its key), the report decode.
 */

#include "freetouch/freetouch.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                            \
	do {                                                                                                       \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                         \
			failures++;                                                                                        \
		}                                                                                                      \
	} while (0)

// key 00..0f, poll 0x12345678 answered two slots late, report bytes 0x40..0x58, encrypted with
// Python's cryptography AESCCM (tag length 4, associated data one zero byte)
static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const uint32_t poll_us = 0x12345678;
static const uint8_t uplink[FREETOUCH_UPLINK_SIZE] = {
    0x00, 0x00, 0x03, 0x00, 0x00, 0x65, 0xc6, 0x28, 0x44, 0x04, 0xe7, 0x17, 0xda, 0x99, 0xbf, 0x85, 0xaa,
    0xf4, 0x4e, 0x3b, 0x6e, 0xb4, 0xd0, 0x9a, 0x33, 0xc0, 0xf9, 0x68, 0xee, 0x19, 0x65, 0xed, 0x59, 0x11};

static void
put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static void
test_decrypt(void)
{
	uint8_t report[FREETOUCH_REPORT_SIZE];
	CHECK(freetouch_decrypt(key, uplink, poll_us, report));
	for (int i = 0; i < FREETOUCH_REPORT_SIZE; i++) {
		CHECK(report[i] == 0x40 + i);
	}
	uint8_t wrong[16];
	memcpy(wrong, key, 16);
	wrong[15] ^= 1;
	CHECK(!freetouch_decrypt(wrong, uplink, poll_us, report));
	// six or more slots late is past what the link allows
	CHECK(!freetouch_decrypt(key, uplink, poll_us - 4 * 2000, report));
}

static void
test_parser(void)
{
	struct freetouch_parser p;
	freetouch_parser_init(&p);
	uint8_t stream[3 * FREETOUCH_RECORD_SIZE + 7];
	memset(stream, 0, sizeof(stream));
	size_t at = 0;

	// an input before the identity: no key yet
	uint8_t *r = stream + at;
	memcpy(r, "CV1I", 4);
	put32(r + 4, 10);
	put32(r + 8, poll_us);
	r[15] = 55;
	r[16] = 34;
	r[17] = (uint8_t)(2 << 4 | 3);
	memcpy(r + 20, uplink, sizeof(uplink));
	at += FREETOUCH_RECORD_SIZE;

	// a few stray bytes, as after a lost USB packet
	memcpy(stream + at, "xyz1234", 7);
	at += 7;

	// the identity: code 9, 21 raw bytes, address then key
	r = stream + at;
	memcpy(r, "CV1S", 4);
	put32(r + 4, FREETOUCH_STATUS_IDENTITY);
	put32(r + 8, 21);
	put32(r + 12, 0x0301);
	const uint8_t address[5] = {0x9e, 0xf1, 0x7d, 0xb4, 0x61};
	memcpy(r + 20, address, 5);
	memcpy(r + 25, key, 16);
	at += FREETOUCH_RECORD_SIZE;

	// the same input again, now with a key
	memcpy(stream + at, stream, FREETOUCH_RECORD_SIZE);
	put32(stream + at + 4, 12);
	at += FREETOUCH_RECORD_SIZE;

	size_t room;
	uint8_t *space = freetouch_parser_space(&p, &room);
	CHECK(room >= at);
	memcpy(space, stream, at);
	freetouch_parser_filled(&p, at);

	struct freetouch_input in;
	struct freetouch_status st;
	struct freetouch_reply reply;
	CHECK(freetouch_parser_next(&p, &in, &st, &reply) == FREETOUCH_RECORD_NO_KEY);
	CHECK(in.session == 3 && in.slot == 2 && in.rssi == 55);

	CHECK(freetouch_parser_next(&p, &in, &st, &reply) == FREETOUCH_RECORD_STATUS);
	CHECK(p.resyncs == 7);
	struct freetouch_identity id;
	CHECK(freetouch_status_identity(&st, &id));
	CHECK(memcmp(id.address, address, 5) == 0 && memcmp(id.key, key, 16) == 0);
	CHECK(id.firmware == 0x0301);
	CHECK(p.have_key);

	CHECK(freetouch_parser_next(&p, &in, &st, &reply) == FREETOUCH_RECORD_INPUT);
	CHECK(in.report[0] == 0x40 && in.report[24] == 0x58);
	CHECK(p.missing == 1); // sequence 10, then 12
	CHECK(freetouch_parser_next(&p, &in, &st, &reply) == FREETOUCH_RECORD_NONE);
}

static void
test_touch_decode(void)
{
	uint8_t r[FREETOUCH_REPORT_SIZE] = {0};
	put32(r, 123456);
	r[4] = 0x34, r[5] = 0x12;               // accel x 0x1234
	r[10] = 0xff, r[11] = 0xff;             // gyro x -1
	r[16] = FREETOUCH_BUTTON_A_X | FREETOUCH_BUTTON_STICK;
	// trigger 1023, grip 512, stick 300 and 700, packed 10 bits each
	const uint32_t trigger = 1023, grip = 512, sx = 300, sy = 700;
	const uint64_t packed = trigger | (uint64_t)grip << 10 | (uint64_t)sx << 20 | (uint64_t)sy << 30;
	for (int i = 0; i < 5; i++) {
		r[17 + i] = (uint8_t)(packed >> (8 * i));
	}
	r[22] = 0x21;
	r[23] = 100;
	struct freetouch_touch t;
	freetouch_touch_decode(r, &t);
	CHECK(t.timestamp == 123456);
	CHECK(t.accel[0] == 0x1234 && t.gyro[0] == -1);
	CHECK(t.buttons == (FREETOUCH_BUTTON_A_X | FREETOUCH_BUTTON_STICK));
	CHECK(t.trigger == trigger && t.grip == grip && t.stick[0] == sx && t.stick[1] == sy);
	CHECK(t.adc_channel == 0x21 && t.adc_value == 100);
}

static void
test_commands(void)
{
	uint8_t b[11];
	CHECK(freetouch_cmd_pair(b, 0) == 1 && b[0] == 'P');
	CHECK(freetouch_cmd_pair(b, 1250) == 4 && b[0] == 'U' && (b[1] | b[2] << 8) == 1250 && b[3] == 'P');
	CHECK(freetouch_cmd_haptic(b, 2, 200, 320) == 5 && b[0] == 'V' && b[1] == 2 && b[2] == 200);
	CHECK(freetouch_cmd_read(b, 0x0a, 3, 0x00140000) == 11 && b[0] == 'O' && b[6] == 'R' && b[9] == 0x14);
	CHECK(freetouch_cmd_identity(b) == 1 && b[0] == 'I');
	CHECK(freetouch_cmd_hop(b, FREETOUCH_HOP_AUTO) == 2 && b[0] == 'H' && b[1] == 255);

	// code 8's b: map bits 32-36, increment in 8-13, auto in 14, sweeping in 15
	struct freetouch_status st = {FREETOUCH_STATUS_CHANNEL_MAP, 0xffffffffu, 0x1f | 13u << 8 | 1u << 14, 0, {0}};
	struct freetouch_hop hop;
	CHECK(freetouch_status_hop(&st, &hop));
	CHECK(hop.increment == 13 && hop.automatic && !hop.sweeping);
	st.code = FREETOUCH_STATUS_FIRMWARE;
	CHECK(!freetouch_status_hop(&st, &hop));
}

int
main(void)
{
	test_decrypt();
	test_parser();
	test_touch_decode();
	test_commands();
	if (failures == 0) {
		printf("libfreetouch: all tests passed\n");
	}
	return failures == 0 ? 0 : 1;
}
