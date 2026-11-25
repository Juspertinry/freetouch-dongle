// Copyright 2026, FreeTouch
/*!
 * @file
 * @brief  Host side of the FreeTouch dongle: port, record parsing, CV1 Touch decryption and commands (docs/PROTOCOL.md).
 *         The dongle reports its own link key (status 9), so a host needs no configuration.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FREETOUCH_VID 0xCAFE
#define FREETOUCH_PID 0x4001
#define FREETOUCH_RECORD_SIZE 54
//! A controller report, decrypted: the headset radio's 28-byte message less its flags and device type.
#define FREETOUCH_REPORT_SIZE 25
//! The encrypted uplink in an input record: 5-byte header, the report, a 4-byte MIC.
#define FREETOUCH_UPLINK_SIZE 34

//! Status record codes.
enum freetouch_status_code
{
	//! After boot: a = 1 when a paired controller was found, b = a fault code (0 none). From 0x0302
	//! also after the radio stopped answering and was restarted: a = 0, b = the fault code.
	FREETOUCH_STATUS_BOOT = 1,
	//! a = polls sent, b = replies (low 16 bits) | overruns << 16, every 5 s.
	FREETOUCH_STATUS_HEARTBEAT = 2,
	//! a = 0x04 | acks << 8 (IR config), b = 0x0f | acks << 8: the controllers' acknowledgements.
	FREETOUCH_STATUS_ACKS = 3,
	//! Per reply slot: a = slot << 24 | replies, b = end times in us after the poll, min | max << 12 | (average / 16) << 24.
	FREETOUCH_STATUS_REPLY_TIMING = 4,
	//! a = firmware revision | (exposure period / 100 us) << 16, b = the chip's FICR DEVICEID0.
	//! After a read command, a = 34 instead and the raw bytes are the request as sent.
	FREETOUCH_STATUS_FIRMWARE = 5,
	//! a = the pairing state (enum freetouch_pairing).
	FREETOUCH_STATUS_PAIRING = 6,
	//! a = a poll's timestamp on the input records' clock, b = the exposure period, every 10 ms.
	FREETOUCH_STATUS_POLL_CLOCK = 7,
	//! a = the channel map in use, bits 0..31, b = bits 32..36 and the hopping (freetouch_status_hop), every 5 s.
	FREETOUCH_STATUS_CHANNEL_MAP = 8,
	//! The dongle's link identity in the raw bytes (freetouch_status_identity), firmware 0x0300+.
	FREETOUCH_STATUS_IDENTITY = 9,
};

enum freetouch_pairing
{
	FREETOUCH_PAIRING_STARTED = 1,
	FREETOUCH_PAIRING_PAIRED = 2,
	FREETOUCH_PAIRING_FAILED = 3,
	FREETOUCH_PAIRING_TIMED_OUT = 4,
};

//! CV1 Touch link sessions (the radio's device type, low nibble).
enum freetouch_session
{
	FREETOUCH_SESSION_LEFT = 2,
	FREETOUCH_SESSION_RIGHT = 3,
};

/*!
 * The dongle's link identity, kept across firmware updates: the radio address the CV1 sensors sync to
 * and the AES-128 key its controllers encrypt with.
 */
struct freetouch_identity
{
	uint8_t address[5];
	uint8_t key[16];
	//! The firmware revision (0x0301 on), 0 for 0x0300, which does not send it here.
	uint16_t firmware;
};

//! `freetouch_cmd_hop`: let the dongle find and keep the increment itself (firmware 0x0301 on).
#define FREETOUCH_HOP_AUTO 255

//! The hopping, from a channel map status record.
struct freetouch_hop
{
	//! The increment in use, 0 when parked on one channel.
	uint8_t increment;
	//! The dongle finds the increment itself (0x0301 on, unless the host fixed one).
	bool automatic;
	//! A sweep for the increment is running.
	bool sweeping;
};

struct freetouch_input
{
	uint32_t sequence;
	//! Dongle clock at the poll the report answered, us.
	uint32_t poll_us;
	//! The link session (enum freetouch_session).
	uint8_t session;
	//! The reply slot it came in, 1.. as paired: two Touch of one hand share a session and differ by it.
	uint8_t slot;
	//! -dBm.
	uint8_t rssi;
	//! The uplink's flags byte (plain): bit 2 acknowledges the last read command.
	uint8_t flags;
	//! Records the dongle could not queue over USB, so far.
	uint16_t reader_drops;
	//! The controller's report, decrypted (freetouch_touch_decode).
	uint8_t report[FREETOUCH_REPORT_SIZE];
};

struct freetouch_status
{
	uint32_t code, a, b, drops;
	//! Raw bytes some codes carry (the identity's, a read request's echo).
	uint8_t raw[34];
};

//! A command reply, raw (a config read's or FRDeviceInfo's frame), and the reply slot it came in.
struct freetouch_reply
{
	uint8_t length;
	uint8_t slot;
	uint8_t frame[48];
};

enum freetouch_record
{
	FREETOUCH_RECORD_NONE,
	FREETOUCH_RECORD_INPUT,
	FREETOUCH_RECORD_STATUS,
	//! An input record that did not decrypt with the key at any slot it may answer.
	FREETOUCH_RECORD_BAD_MIC,
	FREETOUCH_RECORD_REPLY,
	//! An input record that came before the parser had a key (no identity seen yet).
	FREETOUCH_RECORD_NO_KEY,
};

//! Splits the byte stream into records and decrypts the inputs. Zero it, or freetouch_parser_init.
struct freetouch_parser
{
	uint8_t buf[4096];
	size_t have, pos;
	//! Bytes skipped finding a record start again, and gaps in the dongle's sequence numbers.
	uint64_t resyncs, missing;
	uint32_t last_sequence;
	bool have_sequence;
	//! The link key: from an identity record the parser has seen, or freetouch_parser_set_key.
	bool have_key;
	uint8_t key[16];
};

void
freetouch_parser_init(struct freetouch_parser *p);

//! Decrypt with this key from now on (an identity record seen later replaces it).
void
freetouch_parser_set_key(struct freetouch_parser *p, const uint8_t key[16]);

//! Room in the parser's buffer for the next read; call freetouch_parser_filled with what was read into it.
uint8_t *
freetouch_parser_space(struct freetouch_parser *p, size_t *out_size);

void
freetouch_parser_filled(struct freetouch_parser *p, size_t size);

/*!
 * The next whole record, FREETOUCH_RECORD_NONE when the buffer holds none; an identity record sets
 * the key before it is returned. @p out_reply may be NULL to skip replies.
 */
enum freetouch_record
freetouch_parser_next(struct freetouch_parser *p,
                      struct freetouch_input *out_input,
                      struct freetouch_status *out_status,
                      struct freetouch_reply *out_reply);

//! The identity in a status record, false when it is not one (code 9 with 21 raw bytes).
bool
freetouch_status_identity(const struct freetouch_status *s, struct freetouch_identity *out);

//! The hopping in a channel map status record (code 8), false when it is not one.
bool
freetouch_status_hop(const struct freetouch_status *s, struct freetouch_hop *out);

/*!
 * Decrypts a controller uplink answering the poll at @p poll_us. The nonce carries the slot time and a
 * report may be up to five 2 ms slots late, so each is tried.
 */
bool
freetouch_decrypt(const uint8_t key[16],
                  const uint8_t uplink[FREETOUCH_UPLINK_SIZE],
                  uint32_t poll_us,
                  uint8_t out_report[FREETOUCH_REPORT_SIZE]);

//! AES-128-CCM decrypt with a 13-byte nonce and a 4-byte MIC (RFC 3610), as the link runs it.
bool
freetouch_ccm_decrypt(const uint8_t key[16],
                      const uint8_t nonce[13],
                      const uint8_t *aad,
                      size_t aad_len,
                      const uint8_t *cipher,
                      size_t len,
                      const uint8_t mic[4],
                      uint8_t *out);

/*
 * The CV1 Touch report.
 */

#define FREETOUCH_BUTTON_A_X 0x01
#define FREETOUCH_BUTTON_B_Y 0x02
//! Menu on the left controller, the Oculus button on the right.
#define FREETOUCH_BUTTON_SYSTEM 0x04
#define FREETOUCH_BUTTON_STICK 0x08

struct freetouch_touch
{
	//! The controller's clock, us.
	uint32_t timestamp;
	//! Raw IMU counts, the controller's axes; its factory calibration (config reads) scales them.
	int16_t accel[3], gyro[3];
	uint8_t buttons;
	//! 10 bits each; the stick is centred near 512.
	uint16_t trigger, grip, stick[2];
	//! A multiplexed reading per report (capacitive touch sensors, haptics counter, ...) and its channel.
	uint8_t adc_channel;
	uint16_t adc_value;
};

void
freetouch_touch_decode(const uint8_t report[FREETOUCH_REPORT_SIZE], struct freetouch_touch *out);

/*
 * Commands to the dongle, written to its port as they are. Each returns its length.
 */

//! Send the identity now (it also comes when the port is opened, and every 5 s).
size_t
freetouch_cmd_identity(uint8_t out[1]);

//! Pair the next CV1 Touch in pairing mode, within 60 s. @p offset_us 0 picks the reply slot by hand.
size_t
freetouch_cmd_pair(uint8_t out[4], uint16_t offset_us);

//! IR LED pulse width the controllers get (2..15000 us).
size_t
freetouch_cmd_led_width(uint8_t out[5], uint32_t us);

//! Vibrate one session's controllers at 0..255 until told 0; freq >= 320 buzzes at 320 Hz, below it 160.
size_t
freetouch_cmd_haptic(uint8_t out[5], uint8_t session, uint8_t amplitude, uint16_t freq);

//! Power the controllers off (each wakes on a button).
size_t
freetouch_cmd_sleep(uint8_t out[2]);

//! The active-timeout keep-alive on (sent at once, then every 30 min) or off.
size_t
freetouch_cmd_keepalive(uint8_t out[2], bool on);

/*!
 * Fix the hop increment (0 parks on one channel), or FREETOUCH_HOP_AUTO to let the dongle find it.
 * Auto is the default on 0x0301 and later, and the right choice unless testing.
 */
size_t
freetouch_cmd_hop(uint8_t out[2], uint8_t increment);

//! The camera exposure period the polls announce, 5000..65535 us (19200 is the headset's).
size_t
freetouch_cmd_exposure_period(uint8_t out[5], uint32_t us);

/*!
 * One read command to the controllers of @p session, on the next poll: @p opcode 0x0a FRReadConfigBlock
 * with offset | length << 16, or 0x88 FRDeviceInfo with 0. Replies come back as reply records.
 */
size_t
freetouch_cmd_read(uint8_t out[11], uint8_t opcode, uint8_t session, uint32_t arg);

//! Restart into the UF2 bootloader (for a firmware update).
size_t
freetouch_cmd_bootloader(uint8_t out[1]);

/*
 * The port.
 */

typedef struct freetouch_port freetouch_port;

//! The first present dongle's port: "COM5" on Windows, "/dev/ttyACM0" on Linux.
bool
freetouch_port_find(char *out_path, size_t out_len);

//! Opens the port raw with DTR raised: the dongle streams only to an open port.
freetouch_port *
freetouch_port_open(const char *path);

/*!
 * Up to @p size bytes: what is queued at once, else waits up to @p timeout_ms for the first byte.
 * Returns the count (0 on timeout), or -1 on an error (the dongle was unplugged).
 */
int
freetouch_port_read(freetouch_port *port, uint8_t *data, size_t size, int timeout_ms);

//! The count written, or -1 on an error.
int
freetouch_port_write(freetouch_port *port, const uint8_t *data, size_t size);

void
freetouch_port_close(freetouch_port *port);

#ifdef __cplusplus
}
#endif
