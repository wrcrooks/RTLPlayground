/* snmp.c: requests as BER bytes in, answers decoded by an independent reader */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "uip.h"
#include "cmd_parser.h"
#include "snmp.h"
#include "hw_mock.h"
#include <test/support.h>

uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
void *uip_appdata;
uint8_t uip_flags;
struct uip_eth_addr uip_ethaddr = { .addr = { 0x1c, 0x2a, 0xa3, 0, 0, 1 } };
const struct machine machine = { .machine_name = "Test switch", .is_sfp = { [8] = 1 },
	.log_to_phys_port = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }, .phys_to_log_port = { 0, 1, 2, 3, 4, 5, 6, 7, 8 } };
struct machine_runtime machine_detected = { .isRTL8373 = 1 };
char port_names[9][PORT_NAME_SIZE];
char hostname[24] = "sim-switch";
uint8_t cmd_words_len, cmd_words_b[15];
uint8_t outbuf[TCP_OUTBUF_SIZE];
uint16_t slen;
struct uip_udp_conn *uip_udp_conn;
static struct uip_udp_conn listening;
static uint16_t links;
static int sent_len;
static uint8_t sent[1500];

const uint8_t * const HTTP_RESPONSE_JSON = (const uint8_t *)"";
uint16_t strlen_x(const char *s) { return strlen(s); }
uint16_t strtox(uint8_t *d, const char *s) { strcpy((char *)d, s); return strlen(s); }
void char_to_html(char c) { outbuf[slen++] = c; }
void str_code_to_html(const char *s) { while (*s) char_to_html(*s++); }
void bool_to_html(char c) { char_to_html(c ? '1' : '0'); }
void phy_read(uint8_t id, uint8_t dev, uint16_t reg) { (void)dev; (void)reg; SFR_DATA_8 = id == 4 ? 0 : 0x20; }
uint16_t port_link_mask(void) { return links; }
struct uip_udp_conn *uip_udp_new(uip_ipaddr_t *ip, uint16_t rport)
{
	memcpy(listening.ripaddr, ip, 4);
	listening.rport = rport;
	return &listening;
}
void uip_send(const void *p, uint16_t len) { sent_len = len; memcpy(sent, p, len); }

/* ---- requests ---- */
static uint8_t req[1500];
static int rn;

static void tag(int at, int t) { memmove(req + at + 4, req + at, rn - at); req[at] = t; req[at + 1] = 0x82;
	req[at + 2] = (rn - at) >> 8; req[at + 3] = rn - at; rn += 4; }

static void oid(const char *s, uint8_t *o, int *n)
{
	unsigned v[32] = { 0 }, k = 0;
	for (const char *p = s; *p; ) { v[k] = 0; while (*p >= '0' && *p <= '9') v[k] = v[k] * 10 + *p++ - '0'; k++; if (*p == '.') p++; }
	*n = 0;
	o[(*n)++] = v[0] * 40 + v[1];
	for (unsigned i = 2; i < k; i++) {
		if (v[i] > 16383) o[(*n)++] = 0x80 | v[i] >> 14;
		if (v[i] > 127) o[(*n)++] = 0x80 | (v[i] >> 7 & 0x7f);
		o[(*n)++] = v[i] & 0x7f;
	}
}

static int ask(int pdu, int a, int b, const char *community, int n, const char **oids)
{
	rn = 0;
	for (int i = 0; i < n; i++) {
		int at = rn, l;
		uint8_t o[64];
		oid(oids[i], o, &l);
		req[rn++] = 6; req[rn++] = l; memcpy(req + rn, o, l); rn += l;
		req[rn++] = 5; req[rn++] = 0;
		tag(at, 0x30);
	}
	tag(0, 0x30);
	uint8_t h[] = { 2, 4, 0x11, 0x22, 0x33, 0x44, 2, 1, a, 2, 1, b };
	memmove(req + sizeof(h), req, rn); memcpy(req, h, sizeof(h)); rn += sizeof(h);
	tag(0, pdu);
	int cl = strlen(community);
	memmove(req + 5 + cl, req, rn);
	req[0] = 2; req[1] = 1; req[2] = 1; req[3] = 4; req[4] = cl; memcpy(req + 5, community, cl); rn += 5 + cl;
	tag(0, 0x30);
	uip_appdata = uip_buf + UIP_LLH_LEN + UIP_IPUDPH_LEN;
	memcpy(uip_buf + UIP_LLH_LEN + 12, "\xc0\xa8\x0a\x64", 4);
	memcpy(uip_buf + UIP_LLH_LEN + 20, "\xab\xcd", 2);
	memcpy(uip_appdata, req, rn);
	uip_len = rn;
	uip_flags = UIP_NEWDATA;
	uip_udp_conn = &listening;
	sent_len = 0;
	snmp_callback(listening.lport);
	return sent_len;
}

/* ---- answers ---- */
static struct { char oid[80]; int type, len; uint8_t v[130]; } var[256];
static int nvar, err, erridx, bad;

static int rd(const uint8_t **p, const uint8_t *end, int *len)
{
	int t = *(*p)++;
	*len = *(*p)++;
	if (*len == 0x82) { *len = (*p)[0] << 8 | (*p)[1]; *p += 2; }
	if (*len > end - *p) bad = 1;
	return t;
}

static int decode(void)
{
	const uint8_t *p = sent, *end = sent + sent_len, *ve;
	int l;
	nvar = bad = 0;
	if (rd(&p, end, &l) != 0x30 || p + l != end) return 0;
	if (rd(&p, end, &l) != 2 || l != 1 || *p != 1) return 0;
	p += l;
	if (rd(&p, end, &l) != 4) return 0;
	p += l;
	if (rd(&p, end, &l) != 0xa2 || p + l != end) return 0;
	if (rd(&p, end, &l) != 2 || l != 4 || memcmp(p, "\x11\x22\x33\x44", 4)) return 0;
	p += l;
	rd(&p, end, &l); err = *p; p += l;
	rd(&p, end, &l); erridx = *p; p += l;
	if (rd(&p, end, &l) != 0x30 || p + l != end) return 0;
	while (p < end) {
		if (rd(&p, end, &l) != 0x30) return 0;
		ve = p + l;
		if (rd(&p, ve, &l) != 6) return 0;
		int n = sprintf(var[nvar].oid, "%d.%d", p[0] / 40, p[0] % 40);
		for (int i = 1; i < l; i++) n += sprintf(var[nvar].oid + n, ".%d", p[i]);
		p += l;
		var[nvar].type = rd(&p, ve, &l);
		var[nvar].len = l;
		memcpy(var[nvar].v, p, l);
		var[nvar].v[l] = 0;
		p += l;
		if (p != ve) return 0;
		nvar++;
	}
	return !bad;
}

static uint64_t num(int i) { uint64_t v = 0; for (int k = 0; k < var[i].len; k++) v = v << 8 | var[i].v[k]; return v; }
static int get(const char *o) { const char *q[] = { o }; return ask(0xa0, 0, 0, "public", 1, q) && decode() && nvar == 1; }

static void cmd(const char *s)
{
	strcpy((char *)cmd_buffer, s);
	cmd_words_len = 0;
	for (int i = 0; s[i]; i++)
		if (s[i] != ' ' && (!i || s[i - 1] == ' '))
			cmd_words_b[cmd_words_len++] = i;
	err_status = 0;
	snmp_cmd();
}

int main(void)
{
	hw_reset();
	snmp_init();
	ask(0xa0, 0, 0, "public", 0, NULL);
	CHECK(!sent_len, "off until it is turned on");
	cmd("snmp location test bench");
	cmd("snmp on");
	CHECK(listening.lport == HTONS(161) && !listening.rport, "listens on port 161");
	links = 1 << 2 | 1 << 8;
	hw_reg_set(RTL837X_REG_LINKS, 0x200);		/* logical port 2: 1G */
	hw_reg_set(RTL837X_REG_LINKS_89, 4);		/* port 8: 10G */
	hw_reg_set(RTL837X_REG_SEC_COUNTER, 12345);
	hw_counter_set(2, 0, 0x100000123ull);
	hw_counter_set(2, 3, 0xfffffff0ull);
	hw_counter_set(2, 4, 0x20);
	hw_counter_set(2, 48, 0x700000009ull);
	strcpy(port_names[2], "uplink");

	CHECK(get("1.3.6.1.2.1.1.5.0") && !strcmp((char *)var[0].v, "sim-switch"), "sysName is the hostname");
	CHECK(uip_udp_conn != &listening && uip_udp_conn->lport == HTONS(161)
	      && !memcmp(&uip_udp_conn->rport, "\xab\xcd", 2) && !memcmp(uip_udp_conn->ripaddr, "\xc0\xa8\x0a\x64", 4)
	      && !listening.rport, "answers the sender, keeps listening to everyone");
	CHECK(get("1.3.6.1.2.1.1.6.0") && !strcmp((char *)var[0].v, "test bench"), "sysLocation with a space");
	CHECK(get("1.3.6.1.2.1.1.1.0") && strstr((char *)var[0].v, "Test switch"), "sysDescr");
	CHECK(get("1.3.6.1.2.1.1.3.0") && var[0].type == 0x43 && num(0) == 1234500, "sysUpTime");
	CHECK(get("1.3.6.1.2.1.2.1.0") && num(0) == 9, "ifNumber");
	CHECK(get("1.3.6.1.2.1.2.2.1.10.3") && var[0].type == 0x41 && num(0) == 0x123, "ifInOctets is the low half");
	CHECK(get("1.3.6.1.2.1.31.1.1.1.6.3") && var[0].type == 0x46 && num(0) == 0x100000123ull, "ifHCInOctets has all 64 bits");
	CHECK(get("1.3.6.1.2.1.2.2.1.12.3") && num(0) == 0x10, "ifInNUcastPkts wraps");
	CHECK(get("1.3.6.1.2.1.31.1.1.1.2.3") && num(0) == 0xfffffff0 && var[0].len == 5, "a set top bit gets its leading zero");
	CHECK(get("1.3.6.1.2.1.2.2.1.14.3") && num(0) == 7, "ifInErrors");
	CHECK(get("1.3.6.1.2.1.2.2.1.20.3") && num(0) == 9, "ifOutErrors");
	CHECK(get("1.3.6.1.2.1.2.2.1.5.3") && num(0) == 1000000000, "ifSpeed");
	CHECK(get("1.3.6.1.2.1.2.2.1.5.9") && num(0) == 0xffffffff, "ifSpeed of 10G is the maximum");
	CHECK(get("1.3.6.1.2.1.31.1.1.1.15.9") && num(0) == 10000, "ifHighSpeed of 10G");
	CHECK(get("1.3.6.1.2.1.2.2.1.8.3") && num(0) == 1 && get("1.3.6.1.2.1.2.2.1.8.1") && num(0) == 2, "ifOperStatus");
	CHECK(get("1.3.6.1.2.1.2.2.1.7.5") && num(0) == 2 && get("1.3.6.1.2.1.2.2.1.7.1") && num(0) == 1, "ifAdminStatus");
	CHECK(get("1.3.6.1.2.1.31.1.1.1.18.3") && !strcmp((char *)var[0].v, "uplink"), "ifAlias");
	CHECK(get("1.3.6.1.2.1.31.1.1.1.18.4") && !strcmp((char *)var[0].v, "Port 4"), "ifAlias without a name");

	CHECK(get("1.3.6.1.2.1.1.5.1") && var[0].type == 0x81, "noSuchInstance in a scalar");
	CHECK(get("1.3.6.1.2.1.2.2.1.2.10") && var[0].type == 0x81, "noSuchInstance past the last port");
	CHECK(get("1.3.6.1.2.1.2.2.1.2") && var[0].type == 0x81, "noSuchInstance for a bare column");
	CHECK(get("1.3.6.1.4.1.9") && var[0].type == 0x80 && !strcmp(var[0].oid, "1.3.6.1.4.1.9"), "noSuchObject");

	/* walk with GetNext */
	char at[80] = "1.3";
	int n = 0, ordered = 1, sys = 0;
	uint8_t last[64]; int ll = 0;
	for (; n < 1000; n++) {
		const char *q[] = { at };
		if (!ask(0xa1, 0, 0, "public", 1, q) || !decode() || nvar != 1 || var[0].type == 0x82)
			break;
		strcpy(at, var[0].oid);
		uint8_t o[64]; int l;
		oid(at, o, &l);
		int c = memcmp(last, o, ll < l ? ll : l);
		ordered &= c < 0 || (!c && ll < l);
		memcpy(last, o, l); ll = l;
		sys += !strncmp(at, "1.3.6.1.2.1.1.", 14);
	}
	CHECK(n == 7 + 1 + 9 * 20 + 9 * 16 && ordered && var[0].type == 0x82 && sys == 7, "GetNext walks every instance once, in order");
	{ const char *q[] = { "1.3.6.1.2.1.2.2.1.2.200" };
	CHECK(ask(0xa1, 0, 0, "public", 1, q) && decode() && !strcmp(var[0].oid, "1.3.6.1.2.1.2.2.1.3.1"), "GetNext after a two-byte sub-identifier"); }
	{ const char *q[] = { "1.3.6.1.2.1.2.2.1.2" };
	CHECK(ask(0xa1, 0, 0, "public", 1, q) && decode() && !strcmp(var[0].oid, "1.3.6.1.2.1.2.2.1.2.1"), "GetNext of a bare column"); }

	/* GetBulk */
	{ const char *q[] = { "1.3.6.1.2.1.1.5", "1.3.6.1.2.1.2.2.1.2", "1.3.6.1.2.1.2.2.1.10" };
	CHECK(ask(0xa5, 1, 3, "public", 3, q) && decode() && nvar == 7 && !strcmp(var[0].oid, "1.3.6.1.2.1.1.5.0")
	      && !strcmp(var[1].oid, "1.3.6.1.2.1.2.2.1.2.1") && !strcmp(var[2].oid, "1.3.6.1.2.1.2.2.1.10.1")
	      && !strcmp(var[5].oid, "1.3.6.1.2.1.2.2.1.2.3") && !strcmp(var[6].oid, "1.3.6.1.2.1.2.2.1.10.3"),
	      "GetBulk: one non-repeater, two columns, three rounds"); }
	{ const char *q[] = { "1.3" };
	CHECK(ask(0xa5, 0, 100, "public", 1, q) && sent_len <= 1400 && decode() && nvar > 30 && !err, "GetBulk fills the answer, no more"); }
	{ const char *q[] = { "1.3.6.1.2.1.31.1.1.1.18.8" };
	CHECK(ask(0xa5, 0, 10, "public", 1, q) && decode() && nvar == 2 && var[0].type == 4 && var[1].type == 0x82, "GetBulk ends at the end of the MIB"); }
	{ const char *q[] = { "1.3.6.1.2.1.31.2", "1.3.6.1.2.1.31.1.1.1.18.8" };
	CHECK(ask(0xa5, 0, 4, "public", 2, q) && decode() && var[0].type == 0x82 && !strcmp(var[0].oid, "1.3.6.1.2.1.31.2")
	      && var[1].type == 4, "GetBulk beyond the end echoes the name"); }
	{ const char *q[60]; for (int i = 0; i < 60; i++) q[i] = "1.3.6.1.2.1.1.1.0";
	CHECK(ask(0xa0, 0, 0, "public", 45, q) && decode() && err == 1 && !nvar, "tooBig"); }

	{ const char *q[] = { "1.3.6.1.2.1.1.5.0" };
	CHECK(!ask(0xa0, 0, 0, "wrong", 1, q) && !ask(0xa0, 0, 0, "publicx", 1, q) && !ask(0xa0, 0, 0, "publi", 1, q), "another community gets no answer");
	CHECK(ask(0xa3, 0, 0, "public", 1, q) && decode() && err == 17 && erridx == 1, "writes are refused");
	cmd("snmp community secret1");
	CHECK(!ask(0xa0, 0, 0, "public", 1, q) && ask(0xa0, 0, 0, "secret1", 1, q), "the community can be changed");
	/* every truncation and a byte flip anywhere: no crash, no overrun */
	ask(0xa5, 0, 20, "secret1", 1, q);
	uint8_t good[1500]; int gl = rn; memcpy(good, req, gl);
	for (int cut = 0; cut < gl; cut++) { memcpy(uip_appdata, good, cut); uip_len = cut; snmp_callback(listening.lport); }
	for (int i = 0; i < gl; i++)
		for (int v = 0; v < 256; v++) {
			memcpy(uip_appdata, good, gl); ((uint8_t *)uip_appdata)[i] = v; uip_len = gl;
			snmp_callback(listening.lport);
		}
	CHECK(1, "truncated and corrupted requests"); }
	cmd("snmp contact a\"b");
	CHECK(err_status && !snmp_cfg.contact[0], "a quote is refused");
	cmd("snmp location 0123456789012345678901234567890123456789012345678");
	CHECK(err_status && !strcmp(snmp_cfg.location, "test bench"), "49 characters are refused");
	slen = 0;
	send_snmp();
	outbuf[slen] = 0;
	CHECK(!strcmp((char *)outbuf, "{\"enabled\":1,\"community\":\"secret1\",\"location\":\"test bench\",\"contact\":\"\"}"), "/snmp.json");

	printf("%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed != 0;
}
