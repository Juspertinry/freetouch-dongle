// Copyright 2026, FreeTouch
/*!
 * @file
 * @brief  The dongle's stream: records, the link's AES-CCM, the Touch report, commands. See freetouch.h.
 */

#include "freetouch/freetouch.h"

#include <string.h>

/*
 * AES-128, the forward cipher only: CCM needs nothing else.
 */

static const uint8_t aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

struct aes128
{
	uint8_t round_key[176];
};

static void
aes128_init(struct aes128 *a, const uint8_t key[16])
{
	static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
	memcpy(a->round_key, key, 16);
	for (int i = 4; i < 44; i++) {
		uint8_t t[4];
		memcpy(t, a->round_key + (i - 1) * 4, 4);
		if (i % 4 == 0) {
			const uint8_t first = t[0];
			t[0] = (uint8_t)(aes_sbox[t[1]] ^ rcon[i / 4 - 1]);
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[first];
		}
		for (int j = 0; j < 4; j++) {
			a->round_key[i * 4 + j] = (uint8_t)(a->round_key[(i - 4) * 4 + j] ^ t[j]);
		}
	}
}

static uint8_t
xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

static void
aes128_encrypt(const struct aes128 *a, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	for (int i = 0; i < 16; i++) {
		s[i] = (uint8_t)(in[i] ^ a->round_key[i]);
	}
	for (int round = 1; round <= 10; round++) {
		// SubBytes and ShiftRows; the state is column major.
		uint8_t t[16];
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				t[c * 4 + r] = aes_sbox[s[((c + r) % 4) * 4 + r]];
			}
		}
		if (round != 10) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = t + c * 4;
				const uint8_t all = (uint8_t)(col[0] ^ col[1] ^ col[2] ^ col[3]);
				const uint8_t first = col[0];
				col[0] ^= (uint8_t)(all ^ xtime((uint8_t)(col[0] ^ col[1])));
				col[1] ^= (uint8_t)(all ^ xtime((uint8_t)(col[1] ^ col[2])));
				col[2] ^= (uint8_t)(all ^ xtime((uint8_t)(col[2] ^ col[3])));
				col[3] ^= (uint8_t)(all ^ xtime((uint8_t)(col[3] ^ first)));
			}
		}
		for (int i = 0; i < 16; i++) {
			s[i] = (uint8_t)(t[i] ^ a->round_key[round * 16 + i]);
		}
	}
	memcpy(out, s, 16);
}

/*
 * CCM (RFC 3610) with a 13-byte nonce (L = 2) and a 4-byte MIC, as the link runs it.
 */

bool
freetouch_ccm_decrypt(const uint8_t key[16],
                      const uint8_t nonce[13],
                      const uint8_t *aad,
                      size_t aad_len,
                      const uint8_t *cipher,
                      size_t len,
                      const uint8_t mic[4],
                      uint8_t *out)
{
	if (len > 0xffff || aad_len > 14) {
		return false;
	}
	struct aes128 a;
	aes128_init(&a, key);

	uint8_t ctr[16] = {0}, s[16];
	ctr[0] = 1; // L - 1
	memcpy(ctr + 1, nonce, 13);

	for (size_t done = 0, i = 1; done < len; i++) {
		ctr[14] = (uint8_t)(i >> 8);
		ctr[15] = (uint8_t)i;
		aes128_encrypt(&a, ctr, s);
		for (size_t j = 0; j < 16 && done < len; j++, done++) {
			out[done] = (uint8_t)(cipher[done] ^ s[j]);
		}
	}

	// CBC-MAC over B0, the associated data (one block at most here) and the plaintext.
	uint8_t x[16], b[16];
	b[0] = (uint8_t)((aad_len > 0 ? 0x40 : 0x00) | (((4 - 2) / 2) << 3) | 1);
	memcpy(b + 1, nonce, 13);
	b[14] = (uint8_t)(len >> 8);
	b[15] = (uint8_t)len;
	aes128_encrypt(&a, b, x);
	if (aad_len > 0) {
		memset(b, 0, sizeof(b));
		b[0] = (uint8_t)(aad_len >> 8);
		b[1] = (uint8_t)aad_len;
		memcpy(b + 2, aad, aad_len);
		for (int j = 0; j < 16; j++) {
			b[j] ^= x[j];
		}
		aes128_encrypt(&a, b, x);
	}
	for (size_t done = 0; done < len; done += 16) {
		memset(b, 0, sizeof(b));
		const size_t n = len - done < 16 ? len - done : 16;
		memcpy(b, out + done, n);
		for (int j = 0; j < 16; j++) {
			b[j] ^= x[j];
		}
		aes128_encrypt(&a, b, x);
	}

	ctr[14] = ctr[15] = 0;
	aes128_encrypt(&a, ctr, s);
	uint8_t diff = 0;
	for (int j = 0; j < 4; j++) {
		diff |= (uint8_t)((x[j] ^ s[j]) ^ mic[j]);
	}
	return diff == 0;
}

bool
freetouch_decrypt(const uint8_t key[16],
                  const uint8_t uplink[FREETOUCH_UPLINK_SIZE],
                  uint32_t poll_us,
                  uint8_t out_report[FREETOUCH_REPORT_SIZE])
{
	const uint8_t aad = 0;
	// The nonce: a fixed prefix, then the 2 ms poll slot the report answers and poll byte 11, which
	// the headset sends as 0 (firmware before 0x020B sent 1, tried second).
	for (unsigned attempt = 0; attempt < 12; attempt++) {
		const unsigned late = attempt % 6;
		const uint64_t t = ((uint64_t)poll_us + late * 2000ull) | ((uint64_t)(attempt < 6 ? 0 : 1) << 32);
		uint8_t nonce[13] = {0, 0, 0, 0, 0, 0x5c, 0xcf};
		for (int i = 0; i < 6; i++) {
			nonce[7 + i] = (uint8_t)(t >> (8 * i));
		}
		if (freetouch_ccm_decrypt(key, nonce, &aad, 1, uplink + 5, FREETOUCH_REPORT_SIZE, uplink + 30,
		                          out_report)) {
			return true;
		}
	}
	return false;
}

/*
 * Records.
 */

static uint32_t
rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void
freetouch_parser_init(struct freetouch_parser *p)
{
	memset(p, 0, sizeof(*p));
}

void
freetouch_parser_set_key(struct freetouch_parser *p, const uint8_t key[16])
{
	memcpy(p->key, key, 16);
	p->have_key = true;
}

uint8_t *
freetouch_parser_space(struct freetouch_parser *p, size_t *out_size)
{
	if (p->pos > 0) {
		memmove(p->buf, p->buf + p->pos, p->have - p->pos);
		p->have -= p->pos;
		p->pos = 0;
	}
	*out_size = sizeof(p->buf) - p->have;
	return p->buf + p->have;
}

void
freetouch_parser_filled(struct freetouch_parser *p, size_t size)
{
	p->have += size;
}

bool
freetouch_status_identity(const struct freetouch_status *s, struct freetouch_identity *out)
{
	if (s->code != FREETOUCH_STATUS_IDENTITY || s->a != sizeof(out->address) + sizeof(out->key)) {
		return false;
	}
	memcpy(out->address, s->raw, sizeof(out->address));
	memcpy(out->key, s->raw + sizeof(out->address), sizeof(out->key));
	out->firmware = (uint16_t)s->b;
	return true;
}

bool
freetouch_status_hop(const struct freetouch_status *s, struct freetouch_hop *out)
{
	if (s->code != FREETOUCH_STATUS_CHANNEL_MAP) {
		return false;
	}
	out->increment = (uint8_t)((s->b >> 8) & 0x3f);
	out->automatic = (s->b >> 14) & 1;
	out->sweeping = (s->b >> 15) & 1;
	return true;
}

enum freetouch_record
freetouch_parser_next(struct freetouch_parser *p,
                      struct freetouch_input *out_input,
                      struct freetouch_status *out_status,
                      struct freetouch_reply *out_reply)
{
	while (p->have - p->pos >= FREETOUCH_RECORD_SIZE) {
		const uint8_t *r = p->buf + p->pos;

		if (memcmp(r, "CV1S", 4) == 0) {
			p->pos += FREETOUCH_RECORD_SIZE;
			out_status->code = rd32(r + 4);
			out_status->a = rd32(r + 8);
			out_status->b = rd32(r + 12);
			out_status->drops = rd32(r + 16);
			memcpy(out_status->raw, r + 20, sizeof(out_status->raw));
			struct freetouch_identity id;
			if (freetouch_status_identity(out_status, &id)) {
				freetouch_parser_set_key(p, id.key);
			}
			return FREETOUCH_RECORD_STATUS;
		}
		if (memcmp(r, "CV1R", 4) == 0) {
			p->pos += FREETOUCH_RECORD_SIZE;
			if (out_reply == NULL) {
				continue;
			}
			out_reply->length = r[4] > sizeof(out_reply->frame) ? sizeof(out_reply->frame) : r[4];
			out_reply->slot = r[53];
			memcpy(out_reply->frame, r + 5, out_reply->length);
			return FREETOUCH_RECORD_REPLY;
		}
		if (memcmp(r, "CV1I", 4) != 0) {
			// Lost alignment: slide on to the next magic.
			p->pos++;
			p->resyncs++;
			continue;
		}
		p->pos += FREETOUCH_RECORD_SIZE;

		const uint32_t sequence = rd32(r + 4);
		if (p->have_sequence && sequence - p->last_sequence > 1) {
			p->missing += sequence - p->last_sequence - 1;
		}
		p->last_sequence = sequence;
		p->have_sequence = true;

		out_input->sequence = sequence;
		out_input->poll_us = rd32(r + 8);
		out_input->rssi = r[15];
		out_input->slot = r[17] >> 4;
		out_input->reader_drops = (uint16_t)(r[18] | r[19] << 8);
		out_input->session = r[20 + 2];
		out_input->flags = r[20 + 4];
		if (!p->have_key) {
			return FREETOUCH_RECORD_NO_KEY;
		}
		if (!freetouch_decrypt(p->key, r + 20, out_input->poll_us, out_input->report)) {
			return FREETOUCH_RECORD_BAD_MIC;
		}
		return FREETOUCH_RECORD_INPUT;
	}
	return FREETOUCH_RECORD_NONE;
}

/*
 * The CV1 Touch report: the headset radio's message after its flags and device type.
 */

void
freetouch_touch_decode(const uint8_t report[FREETOUCH_REPORT_SIZE], struct freetouch_touch *out)
{
	const uint8_t *r = report;
	out->timestamp = rd32(r);
	for (int i = 0; i < 3; i++) {
		out->accel[i] = (int16_t)(r[4 + 2 * i] | r[5 + 2 * i] << 8);
		out->gyro[i] = (int16_t)(r[10 + 2 * i] | r[11 + 2 * i] << 8);
	}
	out->buttons = r[16];
	// trigger, grip and the stick's two axes: four 10-bit fields packed into five bytes
	const uint8_t *t = r + 17;
	out->trigger = (uint16_t)(t[0] | (t[1] & 0x03) << 8);
	out->grip = (uint16_t)((t[1] & 0xfc) >> 2 | (t[2] & 0x0f) << 6);
	out->stick[0] = (uint16_t)((t[2] & 0xf0) >> 4 | (t[3] & 0x3f) << 4);
	out->stick[1] = (uint16_t)((t[3] & 0xc0) >> 6 | t[4] << 2);
	out->adc_channel = r[22];
	out->adc_value = (uint16_t)(r[23] | r[24] << 8);
}

/*
 * Commands.
 */

static size_t
put32(uint8_t *out, uint8_t cmd, uint32_t v)
{
	out[0] = cmd;
	for (int i = 0; i < 4; i++) {
		out[1 + i] = (uint8_t)(v >> (8 * i));
	}
	return 5;
}

size_t
freetouch_cmd_identity(uint8_t out[1])
{
	out[0] = 'I';
	return 1;
}

size_t
freetouch_cmd_pair(uint8_t out[4], uint16_t offset_us)
{
	if (offset_us == 0) {
		out[0] = 'P';
		return 1;
	}
	// 'U' sets the reply offset this pairing writes to the controller (400..1620 us)
	out[0] = 'U';
	out[1] = (uint8_t)offset_us;
	out[2] = (uint8_t)(offset_us >> 8);
	out[3] = 'P';
	return 4;
}

size_t
freetouch_cmd_led_width(uint8_t out[5], uint32_t us)
{
	return put32(out, 'L', us);
}

size_t
freetouch_cmd_haptic(uint8_t out[5], uint8_t session, uint8_t amplitude, uint16_t freq)
{
	out[0] = 'V';
	out[1] = session;
	out[2] = amplitude;
	out[3] = (uint8_t)freq;
	out[4] = (uint8_t)(freq >> 8);
	return 5;
}

size_t
freetouch_cmd_sleep(uint8_t out[2])
{
	out[0] = 'Z';
	out[1] = 0;
	return 2;
}

size_t
freetouch_cmd_keepalive(uint8_t out[2], bool on)
{
	out[0] = 'A';
	out[1] = on ? 1 : 0;
	return 2;
}

size_t
freetouch_cmd_hop(uint8_t out[2], uint8_t increment)
{
	out[0] = 'H';
	out[1] = increment;
	return 2;
}

size_t
freetouch_cmd_exposure_period(uint8_t out[5], uint32_t us)
{
	return put32(out, 'X', us);
}

size_t
freetouch_cmd_read(uint8_t out[11], uint8_t opcode, uint8_t session, uint32_t arg)
{
	// 'O' opcode, 'S' session, 'Y' 0xff keeps the poll tick, then 'R' sends it with its argument.
	out[0] = 'O';
	out[1] = opcode;
	out[2] = 'S';
	out[3] = session;
	out[4] = 'Y';
	out[5] = 0xff;
	put32(out + 6, 'R', arg);
	return 11;
}

size_t
freetouch_cmd_bootloader(uint8_t out[1])
{
	out[0] = 'b';
	return 1;
}
