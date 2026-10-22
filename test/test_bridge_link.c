/*
 * Mod-chip bridge link framing: modchip/bridge/link.c compiled unmodified.
 *
 * Every message the encoder produces must decode, through an independent
 * COBS decoder here and through link.c's own receiver, to the same type,
 * seq and payload with a valid CRC, for the payload shapes that stress COBS
 * (zero runs, 254-byte blocks, a full-size frame). The receiver must refuse
 * corrupted, truncated, oversized and lost-byte messages.
 *
 * With --emit or --decode it serves test_bridge_link.py, which checks the
 * same framing against the host's Python implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modchip/bridge/link.h"

uint16_t crc_value;

/* What crc16_bank1 in crc16.asm does: one CRC-16 (reflected 0xa001) step */
void crc16_bank1(uint8_t *v)
{
	crc_value ^= *v;
	for (int i = 0; i < 8; i++)
		crc_value = (crc_value & 1) ? (crc_value >> 1) ^ 0xa001 : crc_value >> 1;
}

static uint8_t out[8192];
static size_t out_len;

void uart_putc(uint8_t c)
{
	if (out_len < sizeof(out))
		out[out_len++] = c;
}

static uint8_t msg_buf[LINK_MSG_MAX];
static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL  %s\n", what);
		failures++;
	}
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
	uint16_t saved = crc_value;
	crc_value = 0xffff;
	while (n--)
		crc16_bank1((uint8_t *)p++);
	uint16_t r = crc_value;
	crc_value = saved;
	return r;
}

static void send(uint8_t type, uint8_t seq, uint8_t *payload, size_t n)
{
	out_len = 0;
	link_begin(type, seq);
	link_put_buf(payload, n);
	link_end();
}

/* Reference COBS decoder, written from the specification */
static long cobs_ref(const uint8_t *in, size_t n, uint8_t *dst)
{
	size_t i = 0, o = 0;
	while (i < n) {
		uint8_t code = in[i];
		if (!code || i + code > n)
			return -1;
		for (uint8_t k = 1; k < code; k++) {
			if (!in[i + k])
				return -1;
			dst[o++] = in[i + k];
		}
		i += code;
		if (code != 0xff && i < n)
			dst[o++] = 0;
	}
	return o;
}

static uint8_t feed(const uint8_t *p, size_t n)
{
	uint8_t r = LINK_RX_NONE;
	for (size_t i = 0; i < n; i++) {
		uint8_t s = link_rx_byte(p[i]);
		if (s != LINK_RX_NONE)
			r = s;
	}
	return r;
}

static void roundtrip(const char *name, uint8_t *payload, size_t n)
{
	static uint8_t dec[8192];
	char what[160];

	send(0x41, 0x5a, payload, n);
	snprintf(what, sizeof(what), "%s: one delimiter, at the end", name);
	check(out_len > 0 && out[out_len - 1] == 0 && !memchr(out, 0, out_len - 1), what);

	long d = cobs_ref(out, out_len - 1, dec);
	snprintf(what, sizeof(what), "%s: reference decoder gets type, seq, payload, CRC", name);
	check(d == (long)n + 4 && dec[0] == 0x41 && dec[1] == 0x5a && !memcmp(dec + 2, payload, n)
	      && crc16(dec, d) == 0, what);

	link_rx_reset();
	uint8_t r = feed(out, out_len);
	snprintf(what, sizeof(what), "%s: link_rx_byte decodes it", name);
	check(r == LINK_RX_MSG && link_msg_len == n + 2 && link_msg[0] == 0x41 && link_msg[1] == 0x5a
	      && !memcmp(link_msg + 2, payload, n), what);
}

static void hexline(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
	putchar('\n');
}

/* Payloads shared with test_bridge_link.py, generated identically there */
static size_t vector(int k, uint8_t *p)
{
	static const size_t lens[] = { 0, 1, 1, 253, 254, 255, 508, 509, 300, LINK_MSG_MAX - 4 };
	size_t n = lens[k];
	uint32_t x = 12345 + k;
	for (size_t i = 0; i < n; i++) {
		x = x * 1103515245u + 12345u;
		switch (k) {
		case 1: p[i] = 0; break;
		case 8: p[i] = 0; break;
		case 3: case 4: case 5: case 6: case 7: p[i] = 1 + (x >> 16) % 255; break;
		default: p[i] = x >> 16; break;
		}
	}
	return n;
}
#define N_VECTORS 10

static int emit(void)
{
	static uint8_t p[2048];
	for (int k = 0; k < N_VECTORS; k++) {
		size_t n = vector(k, p);
		send(0x10 + k, k, p, n);
		hexline(out, out_len);
	}
	return 0;
}

/* Lines of hex in, one result per line out */
static int decode(void)
{
	static char line[16384];
	static uint8_t bytes[8192];
	while (fgets(line, sizeof(line), stdin)) {
		size_t n = 0;
		for (char *s = line; s[0] && s[1] && s[0] != '\n'; s += 2) {
			unsigned v;
			sscanf(s, "%2x", &v);
			bytes[n++] = v;
		}
		link_rx_reset();
		uint8_t r = feed(bytes, n);
		if (r == LINK_RX_MSG) {
			printf("MSG ");
			hexline(link_msg, link_msg_len);
		} else {
			printf(r == LINK_RX_ERROR ? "ERROR\n" : "NONE\n");
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t p[2048];

	link_msg = msg_buf;
	if (argc > 1 && !strcmp(argv[1], "--emit"))
		return emit();
	if (argc > 1 && !strcmp(argv[1], "--decode"))
		return decode();

	for (int k = 0; k < N_VECTORS; k++) {
		char name[32];
		size_t n = vector(k, p);
		snprintf(name, sizeof(name), "vector %d (%zu bytes)", k, n);
		roundtrip(name, p, n);
	}
	for (int k = 0; k < 200; k++) {
		size_t n = rand() % (LINK_MSG_MAX - 3);
		for (size_t i = 0; i < n; i++)
			p[i] = (rand() % 4) ? rand() : 0;
		roundtrip("random", p, n);
	}

	/* A message too large for link_msg is refused, and the next one works */
	memset(p, 0x55, sizeof(p));
	send(0x10, 1, p, LINK_MSG_MAX);
	link_rx_reset();
	check(feed(out, out_len) == LINK_RX_ERROR, "oversized message refused");
	send(0x10, 2, p, 10);
	check(feed(out, out_len) == LINK_RX_MSG && link_msg_len == 12, "next message after oversized");

	/* Corrupted: one byte changed, but still non-zero, so COBS stays valid */
	send(0x20, 3, p, 40);
	out[10] ^= 0x01;
	link_rx_reset();
	check(feed(out, out_len) == LINK_RX_ERROR, "corrupted byte fails the CRC");

	/* Lost bytes: the ISR reports an overflow mid-message */
	send(0x20, 4, p, 40);
	link_rx_reset();
	feed(out, 20);
	link_rx_lost();
	check(feed(out + 20, out_len - 20) == LINK_RX_ERROR, "message with lost bytes refused");

	/* Truncated: the tail of one message runs into the next delimiter */
	send(0x20, 5, p, 40);
	link_rx_reset();
	feed(out, 15);
	check(link_rx_byte(0) == LINK_RX_ERROR, "truncated message refused");

	/* Idle delimiters between messages are ignored */
	link_rx_reset();
	check(link_rx_byte(0) == LINK_RX_NONE && link_rx_byte(0) == LINK_RX_NONE, "empty frames ignored");
	send(0x02, 6, p, 0);
	check(feed(out, out_len) == LINK_RX_MSG && link_msg_len == 2, "message after idle delimiters");

	/* Two messages back to back in one buffer */
	{
		static uint8_t two[4096];
		send(0x21, 7, p, 6);
		memcpy(two, out, out_len);
		size_t a = out_len;
		send(0x22, 8, p, 4);
		memcpy(two + a, out, out_len);
		link_rx_reset();
		int got = 0;
		for (size_t i = 0; i < a + out_len; i++)
			if (link_rx_byte(two[i]) == LINK_RX_MSG)
				got += link_msg[0] == (got ? 0x22 : 0x21);
		check(got == 2, "back-to-back messages");
	}

	/* The encoder leaves link_busy as it found it */
	check(!link_busy, "link_busy clear after a message");

	if (failures) {
		printf("%d failures\n", failures);
		return 1;
	}
	printf("PASS  bridge link framing\n");
	return 0;
}
