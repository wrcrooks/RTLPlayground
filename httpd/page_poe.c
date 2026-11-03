/*
 * /poe.json: the RTL8238B PoE controller's state and its raw per-port
 * readings, see rtl8238.h. The web UI converts them (html/app.js).
 *
 * In BANK3 with the loader, because BANK1 (page_impl.c) is full. Code here
 * can't call page_impl.c's helpers or read its constants, which live in
 * BANK1, so it has its own; strtox() is in the common bank.
 * This code is in the Public Domain
 */

#pragma codeseg BANK3
#pragma constseg BANK3

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "machine.h"
#include "uip.h"
#include "page_impl.h"
#include "rtl8238.h"

extern __code const struct machine machine;
extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

static void poe_str(__code const char *s)
{
	slen += strtox(outbuf + slen, s);
}

static void poe_bool(uint8_t v)
{
	outbuf[slen++] = v ? '1' : '0';
}

static void poe_num(uint8_t v)
{
	if (v >= 100)
		outbuf[slen++] = '0' + v / 100;
	if (v >= 10)
		outbuf[slen++] = '0' + (v / 10) % 10;
	outbuf[slen++] = '0' + v % 10;
}

static void poe_hex16(uint16_t v)
{
	uint8_t i, d;

	outbuf[slen++] = '"';
	for (i = 0; i < 4; i++) {
		d = (v >> 12) & 0xf;
		outbuf[slen++] = d < 10 ? '0' + d : 'a' - 10 + d;
		v <<= 4;
	}
	outbuf[slen++] = '"';
}

void send_poe(void) __banked
{
	uint8_t i;

	slen = 0;
	poe_str("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n");
	poe_str("{\"loaded\":");
	poe_bool(pse_loaded);
	poe_str(",\"ports\":[");
	if (machine.pse_rtl8238 && pse_loaded && pse_read_ports()) {
		for (i = 0; i < PSE_PORTS; i++) {
			if (machine.pse_channel[i] > 3)
				continue;
			if (outbuf[slen - 1] == '}')
				outbuf[slen++] = ',';
			poe_str("{\"portNum\":");
			poe_num(i + 1);
			poe_str(",\"det\":");
			poe_num(pse_ports[i].det);
			poe_str(",\"cls\":");
			poe_num(pse_ports[i].cls);
			poe_str(",\"on\":");
			poe_bool(pse_ports[i].on);
			poe_str(",\"en\":");
			poe_bool(pse_ports[i].en);
			poe_str(",\"i\":");
			poe_hex16(pse_ports[i].i_raw);
			poe_str(",\"v\":");
			poe_hex16(pse_ports[i].v_raw);
			outbuf[slen++] = '}';
		}
	}
	poe_str("]}");
}
