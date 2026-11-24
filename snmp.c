/*
 * SNMPv2c agent, read-only, on UDP port 161: the system group, ifTable and
 * ifXTable with the switch's ports and the ASIC's counters. See doc/snmp.md.
 *
 * All sub-identifiers in this MIB are below 128, so its OIDs are the same as
 * numbers and as BER bytes, and comparing a requested OID's BER bytes with
 * them orders it correctly: a larger sub-identifier begins with a byte above
 * 0x80. An instance is an object (a row of snmp_obj) and, in the tables, a
 * front port.
 *
 * The request is read in uip_buf while the answer is built in snmp_out. The
 * functions are not reentrant and pass their operands in XDATA
 * (doc/internal-ram.md).
 */

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_phy.h"
#include "phy.h"
#include "machine.h"
#include "cmd_parser.h"
#include "page_impl.h"
#include "version.h"
#include "uip/uip.h"
#include "snmp.h"

#pragma codeseg BANK3
#pragma constseg BANK3

#define SNMP_PORT	161
#define SNMP_MAX	1400
#define SNMP_REPEATERS	16
#define SNMP_OID_MAX	16

#define T_INT		0x02
#define T_STR		0x04
#define T_OID		0x06
#define T_SEQ		0x30
#define T_COUNTER	0x41
#define T_GAUGE		0x42
#define T_TICKS		0x43
#define T_COUNTER64	0x46
#define T_NO_OBJECT	0x80
#define T_NO_INSTANCE	0x81
#define T_END		0x82

#define PDU_GET		0xa0
#define PDU_NEXT	0xa1
#define PDU_RESPONSE	0xa2
#define PDU_SET		0xa3
#define PDU_BULK	0xa5

#define ERR_TOO_BIG	1
#define ERR_NOT_WRITABLE 17

/* Object groups: what comes before the column, and what instance after it */
#define G_SYSTEM	0	/* 1.col.0 */
#define G_INTERFACES	1	/* 2.col.0 */
#define G_IF		2	/* 2.2.1.col.port */
#define G_IFX		3	/* 31.1.1.1.col.port */

#define V_DESCR		0
#define V_OBJECT_ID	1
#define V_UPTIME	2
#define V_CONTACT	3
#define V_NAME		4
#define V_LOCATION	5
#define V_SMALL		6	/* arg */
#define V_PORTS		7
#define V_INDEX		8
#define V_IF_DESCR	9
#define V_MTU		10
#define V_SPEED		11
#define V_MAC		12
#define V_ADMIN		13
#define V_OPER		14
#define V_CNT_LOW	15	/* low half of counter arg */
#define V_CNT_HIGH	16	/* high half of counter arg */
#define V_CNT_PAIR	17	/* low halves of counters arg and arg + 1 */
#define V_CNT64		18
#define V_HIGH_SPEED	19
#define V_ALIAS		20
#define V_IF_NAME	21

/* The ASIC's counters (STAT_GET) */
#define C_IN_OCTETS	0
#define C_OUT_OCTETS	1
#define C_IN_UCAST	2
#define C_IN_MCAST	3
#define C_IN_BCAST	4
#define C_OUT_UCAST	5
#define C_OUT_MCAST	6
#define C_OUT_BCAST	7
#define C_DISCARDS	8	/* high half out, low half in */
#define C_ERRORS	48	/* high half in, low half out */

struct snmp_object {
	uint8_t group, col, type, val, arg;
};

static __code const struct snmp_object snmp_obj[] = {
	{ G_SYSTEM, 1, T_STR, V_DESCR, 0 },
	{ G_SYSTEM, 2, T_OID, V_OBJECT_ID, 0 },
	{ G_SYSTEM, 3, T_TICKS, V_UPTIME, 0 },
	{ G_SYSTEM, 4, T_STR, V_CONTACT, 0 },
	{ G_SYSTEM, 5, T_STR, V_NAME, 0 },
	{ G_SYSTEM, 6, T_STR, V_LOCATION, 0 },
	{ G_SYSTEM, 7, T_INT, V_SMALL, 2 },		/* sysServices: datalink */
	{ G_INTERFACES, 1, T_INT, V_PORTS, 0 },
	{ G_IF, 1, T_INT, V_INDEX, 0 },
	{ G_IF, 2, T_STR, V_IF_DESCR, 0 },
	{ G_IF, 3, T_INT, V_SMALL, 6 },			/* ethernetCsmacd */
	{ G_IF, 4, T_INT, V_MTU, 0 },
	{ G_IF, 5, T_GAUGE, V_SPEED, 0 },
	{ G_IF, 6, T_STR, V_MAC, 0 },
	{ G_IF, 7, T_INT, V_ADMIN, 0 },
	{ G_IF, 8, T_INT, V_OPER, 0 },
	{ G_IF, 9, T_TICKS, V_SMALL, 0 },		/* ifLastChange */
	{ G_IF, 10, T_COUNTER, V_CNT_LOW, C_IN_OCTETS },
	{ G_IF, 11, T_COUNTER, V_CNT_LOW, C_IN_UCAST },
	{ G_IF, 12, T_COUNTER, V_CNT_PAIR, C_IN_MCAST },
	{ G_IF, 13, T_COUNTER, V_CNT_LOW, C_DISCARDS },
	{ G_IF, 14, T_COUNTER, V_CNT_HIGH, C_ERRORS },
	{ G_IF, 15, T_COUNTER, V_SMALL, 0 },		/* ifInUnknownProtos */
	{ G_IF, 16, T_COUNTER, V_CNT_LOW, C_OUT_OCTETS },
	{ G_IF, 17, T_COUNTER, V_CNT_LOW, C_OUT_UCAST },
	{ G_IF, 18, T_COUNTER, V_CNT_PAIR, C_OUT_MCAST },
	{ G_IF, 19, T_COUNTER, V_CNT_HIGH, C_DISCARDS },
	{ G_IF, 20, T_COUNTER, V_CNT_LOW, C_ERRORS },
	{ G_IFX, 1, T_STR, V_IF_NAME, 0 },
	{ G_IFX, 2, T_COUNTER, V_CNT_LOW, C_IN_MCAST },
	{ G_IFX, 3, T_COUNTER, V_CNT_LOW, C_IN_BCAST },
	{ G_IFX, 4, T_COUNTER, V_CNT_LOW, C_OUT_MCAST },
	{ G_IFX, 5, T_COUNTER, V_CNT_LOW, C_OUT_BCAST },
	{ G_IFX, 6, T_COUNTER64, V_CNT64, C_IN_OCTETS },
	{ G_IFX, 7, T_COUNTER64, V_CNT64, C_IN_UCAST },
	{ G_IFX, 8, T_COUNTER64, V_CNT64, C_IN_MCAST },
	{ G_IFX, 9, T_COUNTER64, V_CNT64, C_IN_BCAST },
	{ G_IFX, 10, T_COUNTER64, V_CNT64, C_OUT_OCTETS },
	{ G_IFX, 11, T_COUNTER64, V_CNT64, C_OUT_UCAST },
	{ G_IFX, 12, T_COUNTER64, V_CNT64, C_OUT_MCAST },
	{ G_IFX, 13, T_COUNTER64, V_CNT64, C_OUT_BCAST },
	{ G_IFX, 15, T_GAUGE, V_HIGH_SPEED, 0 },
	{ G_IFX, 17, T_INT, V_SMALL, 1 },		/* ifConnectorPresent: true */
	{ G_IFX, 18, T_STR, V_ALIAS, 0 },
};
#define SNMP_OBJECTS	(sizeof(snmp_obj) / sizeof(snmp_obj[0]))

/* By link code: 10M, 100M, 1G, 500M, 10G, 2.5G, 5G */
static __code const uint16_t snmp_mbps[] = { 10, 100, 1000, 500, 10000, 2500, 5000 };
static __code const uint32_t snmp_bps[] = {
	10000000, 100000000, 1000000000, 500000000, 0xffffffff, 2500000000, 0xffffffff
};

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[15];
extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

__xdata struct snmp_config snmp_cfg;

static __xdata struct uip_udp_conn * __xdata snmp_conn;
static __xdata struct uip_udp_conn snmp_peer;	/* where the answer goes */
static __xdata uip_ipaddr_t snmp_any;

static __xdata uint8_t snmp_out[SNMP_MAX];
static __xdata uint16_t s_n;			/* bytes in snmp_out */
static __xdata uint8_t s_ovf;

static __xdata uint8_t * __xdata s_in;		/* the request */
static __xdata uint16_t s_at, s_end;		/* where reading is, and may go */
static __xdata uint8_t s_tag;
static __xdata uint16_t s_len;

static __xdata uint8_t * __xdata s_oid;		/* requested OID, BER */
static __xdata uint8_t s_oid_len;
static __xdata uint8_t s_c[SNMP_OID_MAX], s_c_len;	/* an instance's OID */
static __xdata uint8_t s_o, s_i;		/* object and front port (0 in a scalar) */
static __xdata uint8_t s_ports;
static __xdata uint8_t s_v[8];			/* a number, big-endian */
static __xdata uint8_t s_next[SNMP_REPEATERS], s_next_i[SNMP_REPEATERS];


/* ---- reading ---- */

/* The tag and length at s_at; s_at moves to the contents, which fit below s_end */
static uint8_t tlv(void)
{
	static __xdata uint8_t l;

	if (s_at + 2 > s_end)
		return 0;
	s_tag = s_in[s_at++];
	l = s_in[s_at++];
	s_len = l;
	if (l & 0x80) {
		l &= 0x7f;
		if (!l || l > 2 || s_at + l > s_end)
			return 0;
		s_len = 0;
		while (l--)
			s_len = (s_len << 8) | s_in[s_at++];
	}
	return s_len <= s_end - s_at;
}


/* A small non-negative INTEGER, saturated at 255; a negative one is 0 */
static uint8_t small_int(void)
{
	static __xdata uint8_t v;

	if (!tlv() || s_tag != T_INT || !s_len)
		return 0;
	v = s_in[s_at + s_len - 1];
	if (s_in[s_at] & 0x80)
		v = 0;
	else if (s_len > 1 && (s_len > 2 || s_in[s_at]))
		v = 255;
	s_at += s_len;
	return v;
}


/* ---- writing ---- */

static void put(uint8_t b)
{
	if (s_n < SNMP_MAX)
		snmp_out[s_n++] = b;
	else
		s_ovf = 1;
}


/* A constructed value whose length is filled in by close_at(), at the s_n returned */
static uint16_t open_tag(uint8_t tag)
{
	put(tag);
	put(0x82);
	put(0);
	put(0);
	return s_n;
}


static __xdata uint16_t s_l;

static void close_at(uint16_t at)
{
	s_l = s_n - at;
	if (s_ovf)
		return;
	snmp_out[at - 2] = s_l >> 8;
	snmp_out[at - 1] = s_l;
}


static void put_str_x(__xdata const char *s)
{
	static __xdata uint8_t n;

	n = strlen_x(s);
	put(T_STR);
	put(n);
	while (n--)
		put(*s++);
}


static void put_code(__code const char *s)
{
	while (*s)
		put(*s++);
}


/* s_v as an unsigned number of type s_tag */
static void put_num(void)
{
	static __xdata uint8_t k;

	for (k = 0; k < 7 && !s_v[k]; k++)
		;
	put(s_tag);
	if (s_v[k] & 0x80) {
		put(9 - k);
		put(0);
	} else {
		put(8 - k);
	}
	while (k < 8)
		put(s_v[k++]);
}


static __xdata uint32_t s_u32;

static void set_u32(void)
{
	s_v[4] = ((__xdata uint8_t *)&s_u32)[3];
	s_v[5] = ((__xdata uint8_t *)&s_u32)[2];
	s_v[6] = ((__xdata uint8_t *)&s_u32)[1];
	s_v[7] = ((__xdata uint8_t *)&s_u32)[0];
}


/* ---- the MIB ---- */

static uint8_t is_table(void)
{
	return snmp_obj[s_o].group >= G_IF;
}


/* The OID of object s_o, port s_i */
static void instance_oid(void)
{
	s_c[0] = 0x2b;		/* 1.3.6.1.2.1 */
	s_c[1] = 6;
	s_c[2] = 1;
	s_c[3] = 2;
	s_c[4] = 1;
	s_c_len = 5;
	switch (snmp_obj[s_o].group) {
	case G_SYSTEM:
		s_c[s_c_len++] = 1;
		break;
	case G_INTERFACES:
		s_c[s_c_len++] = 2;
		break;
	case G_IF:
		s_c[s_c_len++] = 2;
		s_c[s_c_len++] = 2;
		s_c[s_c_len++] = 1;
		break;
	default:
		s_c[s_c_len++] = 31;
		s_c[s_c_len++] = 1;
		s_c[s_c_len++] = 1;
		s_c[s_c_len++] = 1;
	}
	s_c[s_c_len++] = snmp_obj[s_o].col;
	s_c[s_c_len++] = s_i;
}


/* The instance's OID comes after the requested one */
static uint8_t after(void)
{
	static __xdata uint8_t k;

	for (k = 0; k < s_c_len; k++) {
		if (k == s_oid_len)
			return 1;
		if (s_c[k] != s_oid[k])
			return s_c[k] > s_oid[k];
	}
	return 0;
}


/* The first instance after the requested OID, in s_o and s_i; s_o is SNMP_OBJECTS if there is none */
static void find_next(void)
{
	for (s_o = 0; s_o < SNMP_OBJECTS; s_o++) {
		s_i = is_table() ? s_ports : 0;
		instance_oid();
		if (!after())
			continue;
		if (!is_table())
			return;
		for (s_i = 1; s_i < s_ports; s_i++) {
			instance_oid();
			if (after())
				return;
		}
		return;
	}
}


/* From one instance to the next */
static void advance(void)
{
	if (is_table() && s_i < s_ports) {
		s_i++;
		return;
	}
	s_o++;
	s_i = s_o < SNMP_OBJECTS && is_table() ? 1 : 0;
}


/* The requested OID itself: 0, or the exception to answer with */
static uint8_t find_exact(void) __reentrant
{
	static __xdata uint8_t k;

	for (s_o = 0; s_o < SNMP_OBJECTS; s_o++) {
		s_i = 0;
		instance_oid();
		if (s_oid_len < s_c_len - 1)
			continue;
		for (k = 0; k < s_c_len - 1; k++)
			if (s_c[k] != s_oid[k])
				break;
		if (k < s_c_len - 1)
			continue;
		if (s_oid_len != s_c_len)
			return T_NO_INSTANCE;
		s_i = s_oid[k];
		if (is_table() ? (!s_i || s_i > s_ports) : s_i)
			return T_NO_INSTANCE;
		return 0;
	}
	return T_NO_OBJECT;
}


static __xdata uint8_t v_port, v_arg, v_k;

/* Counter v_arg of the port into s_v */
static void counter(void)
{
	STAT_GET(v_arg, v_port);
	reg_read_m(RTL837X_STAT_V_HIGH);
	for (v_k = 0; v_k < 4; v_k++)
		s_v[v_k] = sfr_data[v_k];
	reg_read_m(RTL837X_STAT_V_LOW);
	for (v_k = 0; v_k < 4; v_k++)
		s_v[4 + v_k] = sfr_data[v_k];
}


/* The low half of counter v_arg */
static uint32_t counter_low(void)
{
	counter();
	((__xdata uint8_t *)&s_u32)[0] = s_v[7];
	((__xdata uint8_t *)&s_u32)[1] = s_v[6];
	((__xdata uint8_t *)&s_u32)[2] = s_v[5];
	((__xdata uint8_t *)&s_u32)[3] = s_v[4];
	return s_u32;
}


static uint8_t link_up(void)
{
	return (port_link_mask() >> v_port) & 1;
}


/* Index into snmp_mbps, or 0xff without a link */
static uint8_t link_code(void)
{
	if (!link_up())
		return 0xff;
	if (v_port < 8)
		reg_read_m(RTL837X_REG_LINKS);
	else
		reg_read_m(RTL837X_REG_LINKS_89);
	v_k = sfr_data[3 - ((v_port & 7) >> 1)];
	v_k = (v_port & 1) ? v_k >> 4 : v_k & 0xf;
	return v_k < sizeof(snmp_mbps) / sizeof(snmp_mbps[0]) ? v_k : 0xff;
}


/* The value of s_o, s_i */
static void put_value(void) __reentrant
{
	static __xdata uint16_t at;

	for (v_k = 0; v_k < 8; v_k++)
		s_v[v_k] = 0;
	s_tag = snmp_obj[s_o].type;
	v_arg = snmp_obj[s_o].arg;
	if (is_table())
		v_port = machine.phys_to_log_port[s_i - 1];
	switch (snmp_obj[s_o].val) {
	case V_DESCR:
		put(T_STR);
		put(0);
		at = s_n;
		put_code("RTLPlayground " VERSION_SW ", ");
		put_code(machine.machine_name);
		if (!s_ovf)
			snmp_out[at - 1] = s_n - at;
		return;
	case V_OBJECT_ID:
		put(T_OID);
		put(1);
		put(0);
		return;
	case V_UPTIME:
		reg_read_m(RTL837X_REG_SEC_COUNTER);
		((__xdata uint8_t *)&s_u32)[0] = sfr_data[3];
		((__xdata uint8_t *)&s_u32)[1] = sfr_data[2];
		((__xdata uint8_t *)&s_u32)[2] = sfr_data[1];
		((__xdata uint8_t *)&s_u32)[3] = sfr_data[0];
		/* times 100 */
		s_u32 = (s_u32 << 6) + (s_u32 << 5) + (s_u32 << 2);
		set_u32();
		break;
	case V_CONTACT:
		put_str_x(snmp_cfg.contact);
		return;
	case V_NAME:
		put_str_x(hostname);
		return;
	case V_LOCATION:
		put_str_x(snmp_cfg.location);
		return;
	case V_SMALL:
		s_v[7] = v_arg;
		break;
	case V_PORTS:
		s_v[7] = s_ports;
		break;
	case V_INDEX:
		s_v[7] = s_i;
		break;
	case V_IF_DESCR:
		put(T_STR);
		put(6);
		put_code("Port ");
		put('0' + s_i);
		return;
	case V_IF_NAME:
		put(T_STR);
		put(5);
		put_code("port");
		put('0' + s_i);
		return;
	case V_MTU:
		s_v[6] = 1500 >> 8;
		s_v[7] = 1500 & 0xff;
		break;
	case V_SPEED:
		v_k = link_code();
		if (v_k != 0xff) {
			s_u32 = snmp_bps[v_k];
			set_u32();
		}
		break;
	case V_HIGH_SPEED:
		v_k = link_code();
		if (v_k != 0xff) {
			s_u32 = snmp_mbps[v_k];
			set_u32();
		}
		break;
	case V_MAC:
		put(T_STR);
		put(6);
		for (v_k = 0; v_k < 6; v_k++)
			put(uip_ethaddr.addr[v_k]);
		return;
	case V_ADMIN:
		s_v[7] = 1;
		if (!machine.is_sfp[v_port]) {
			phy_read(v_port, PHY_MMD31, 0xa610);
			if (SFR_DATA_8 != 0x20)
				s_v[7] = 2;
		}
		break;
	case V_OPER:
		s_v[7] = link_up() ? 1 : 2;
		break;
	case V_CNT_LOW:
		counter();
		for (v_k = 0; v_k < 4; v_k++)
			s_v[v_k] = 0;
		break;
	case V_CNT_HIGH:
		counter();
		for (v_k = 0; v_k < 4; v_k++) {
			s_v[4 + v_k] = s_v[v_k];
			s_v[v_k] = 0;
		}
		break;
	case V_CNT_PAIR: {
		static __xdata uint32_t sum;

		sum = counter_low();
		v_arg++;
		sum += counter_low();
		s_u32 = sum;
		for (v_k = 0; v_k < 4; v_k++)
			s_v[v_k] = 0;
		set_u32();
		break;
	}
	case V_CNT64:
		counter();
		break;
	case V_ALIAS:
		if (port_names[v_port][0]) {
			put_str_x(port_names[v_port]);
		} else {
			put(T_STR);
			put(6);
			put_code("Port ");
			put('0' + s_i);
		}
		return;
	}
	put_num();
}


/* ---- requests ---- */

/* A variable binding: the instance s_o, s_i, or the requested OID with the exception */
static void put_binding(uint8_t exception)
{
	static __xdata uint16_t at;
	static __xdata uint8_t k, ex;

	ex = exception;
	at = open_tag(T_SEQ);
	put(T_OID);
	if (ex && s_oid) {
		put(s_oid_len);
		for (k = 0; k < s_oid_len; k++)
			put(s_oid[k]);
	} else {
		instance_oid();
		put(s_c_len);
		for (k = 0; k < s_c_len; k++)
			put(s_c[k]);
	}
	if (ex) {
		put(ex);
		put(0);
	} else {
		put_value();
	}
	close_at(at);
}


/* The next variable binding of the request: its OID */
static uint8_t read_binding(void)
{
	static __xdata uint16_t next, end;

	if (!tlv() || s_tag != T_SEQ)
		return 0;
	next = s_at + s_len;
	end = s_end;
	s_end = next;
	if (!tlv() || s_tag != T_OID || s_len > 127) {
		s_end = end;
		return 0;
	}
	s_oid = s_in + s_at;
	s_oid_len = s_len;
	s_at = next;
	s_end = end;
	return 1;
}


/* Nonzero: the answer, s_n bytes in snmp_out */
static uint8_t request(void)
{
	static __xdata uint16_t msg, pdu_at, req_at, vars_at, err_at, mark;
	static __xdata uint8_t pdu, k, n, non, rep, repeaters, live;

	/* version 2c, community */
	if (!tlv() || s_tag != T_SEQ)
		return 0;
	s_end = s_at + s_len;
	if (small_int() != 1 || !tlv() || s_tag != T_STR)
		return 0;
	n = strlen_x(snmp_cfg.community);
	if (s_len != n)
		return 0;
	for (k = 0; k < n; k++)
		if (s_in[s_at + k] != snmp_cfg.community[k])
			return 0;
	s_n = 0;
	s_ovf = 0;
	msg = open_tag(T_SEQ);
	put(T_INT);
	put(1);
	put(1);
	put(T_STR);
	put(n);
	for (k = 0; k < n; k++)
		put(s_in[s_at + k]);
	s_at += s_len;

	if (!tlv())
		return 0;
	pdu = s_tag;
	if (pdu != PDU_GET && pdu != PDU_NEXT && pdu != PDU_BULK && pdu != PDU_SET)
		return 0;
	s_end = s_at + s_len;
	pdu_at = open_tag(PDU_RESPONSE);
	req_at = s_at;
	if (!tlv() || s_tag != T_INT || s_len > 5)
		return 0;
	s_at += s_len;
	while (req_at < s_at)
		put(s_in[req_at++]);
	non = small_int();
	rep = small_int();
	put(T_INT);
	put(1);
	err_at = s_n;
	put(0);
	put(T_INT);
	put(1);
	put(0);
	if (!tlv() || s_tag != T_SEQ)
		return 0;
	s_end = s_at + s_len;
	vars_at = open_tag(T_SEQ);

	if (pdu == PDU_SET) {
		while (s_at < s_end)
			put(s_in[s_at++]);
		snmp_out[err_at] = ERR_NOT_WRITABLE;
		snmp_out[err_at + 3] = 1;
	} else {
		repeaters = 0;
		mark = s_n;
		while (s_at < s_end) {
			if (!read_binding())
				return 0;
			if (pdu == PDU_GET) {
				put_binding(find_exact());
			} else if (pdu == PDU_NEXT || non) {
				find_next();
				put_binding(s_o == SNMP_OBJECTS ? T_END : 0);
				if (non)
					non--;
			} else if (rep && repeaters < SNMP_REPEATERS) {
				find_next();
				put_binding(s_o == SNMP_OBJECTS ? T_END : 0);
				s_next[repeaters] = s_o;
				s_next_i[repeaters++] = s_i;
			}
			if (s_ovf)
				break;
			mark = s_n;
		}
		/* GetBulk: the further rounds, until everything is at its end or the answer is full */
		s_oid = 0;
		while (!s_ovf && repeaters && --rep) {
			live = 0;
			for (k = 0; k < repeaters && !s_ovf; k++) {
				s_o = s_next[k];
				s_i = s_next_i[k];
				if (s_o < SNMP_OBJECTS)
					advance();
				if (s_o < SNMP_OBJECTS)
					live = 1;
				s_next[k] = s_o;
				s_next_i[k] = s_i;
				if (s_o == SNMP_OBJECTS) {
					s_o = SNMP_OBJECTS - 1;
					s_i = s_ports;
					put_binding(T_END);
				} else {
					put_binding(0);
				}
				if (!s_ovf)
					mark = s_n;
			}
			if (!live)
				break;
		}
		if (s_ovf) {
			s_ovf = 0;
			s_n = mark;
			if (pdu != PDU_BULK) {
				s_n = vars_at;
				snmp_out[err_at] = ERR_TOO_BIG;
			}
		}
	}
	if (s_ovf)
		return 0;
	close_at(vars_at);
	close_at(pdu_at);
	close_at(msg);
	return 1;
}


void snmp_callback(uint16_t lport) __banked
{
	if (!snmp_conn || lport != snmp_conn->lport || !uip_newdata())
		return;
	s_in = (__xdata uint8_t *)uip_appdata;
	s_at = 0;
	s_end = uip_datalen();
	s_ports = MACHINE_MAX_PORT - MACHINE_MIN_PORT + 1;
	/* to whoever asked, so not over the listening connection, which has no peer */
	memcpy(&snmp_peer, snmp_conn, sizeof(snmp_peer));
	memcpy(snmp_peer.ripaddr, &uip_buf[UIP_LLH_LEN + 12], 4);
	memcpy(&snmp_peer.rport, &uip_buf[UIP_LLH_LEN + 20], 2);
	if (!request())
		return;
	uip_udp_conn = &snmp_peer;
	uip_send(snmp_out, s_n);
}


static void snmp_apply(void)
{
	if (snmp_conn) {
		uip_udp_remove(snmp_conn);
		snmp_conn = 0;
	}
	if (!snmp_cfg.on)
		return;
	uip_ipaddr(snmp_any, 0, 0, 0, 0);
	snmp_conn = uip_udp_new(&snmp_any, 0);
	if (!snmp_conn) {
		print_string("SNMP: no free UDP connection\n");
		return;
	}
	uip_udp_bind(snmp_conn, HTONS(SNMP_PORT));
}


void snmp_init(void) __banked
{
	snmp_cfg.on = 0;
	snmp_conn = 0;
	strtox((__xdata uint8_t *)snmp_cfg.community, "public");
	snmp_cfg.location[0] = 0;
	snmp_cfg.contact[0] = 0;
}


/* ---- command and page ---- */

static __xdata uint8_t c_w, c_k;

static uint8_t c_eq(__code const char *s)
{
	static __xdata uint8_t i;

	if (c_w >= cmd_words_len)
		return 0;
	i = cmd_words_b[c_w];
	while (*s)
		if (cmd_buffer[i++] != *s++)
			return 0;
	return cmd_buffer[i] == ' ' || !cmd_buffer[i];
}


static __xdata char * __xdata c_dst;

/* The rest of the line from word 2 on into c_dst */
static uint8_t c_text(void)
{
	static __xdata uint8_t p;

	if (cmd_words_len < 3)
		return 0;
	p = cmd_words_b[2];
	for (c_k = 0; cmd_buffer[p + c_k]; c_k++)
		if (c_k == SNMP_TEXT_MAX || cmd_buffer[p + c_k] < ' ' || cmd_buffer[p + c_k] > '~'
		    || cmd_buffer[p + c_k] == '"' || cmd_buffer[p + c_k] == '\\')
			return 0;
	for (c_k = 0; cmd_buffer[p + c_k]; c_k++)
		c_dst[c_k] = cmd_buffer[p + c_k];
	c_dst[c_k] = 0;
	return 1;
}


void snmp_cmd(void) __banked
{
	static __xdata uint8_t ok;

	if (cmd_words_len == 1) {
		print_string(snmp_cfg.on ? "SNMP on, community " : "SNMP off, community ");
		print_string_x(snmp_cfg.community);
		print_string(", location ");
		print_string_x(snmp_cfg.location);
		print_string(", contact ");
		print_string_x(snmp_cfg.contact);
		write_char('\n');
		return;
	}
	ok = 0;
	c_w = 1;
	if (c_eq("on") && cmd_words_len == 2) {
		ok = snmp_cfg.on = 1;
	} else if (c_eq("off") && cmd_words_len == 2) {
		snmp_cfg.on = 0;
		ok = 1;
	} else if (c_eq("community")) {
		c_dst = snmp_cfg.community;
		ok = c_text();
	} else if (c_eq("location")) {
		c_dst = snmp_cfg.location;
		ok = c_text();
	} else if (c_eq("contact")) {
		c_dst = snmp_cfg.contact;
		ok = c_text();
	}
	if (ok) {
		snmp_apply();
		return;
	}
	err_status = ERR_INVALID_ARGUMENT;
	print_string("snmp [on|off|community <name>|location <text>|contact <text>], up to 48 characters without \" and \\\n");
}


static void j_text(void)
{
	char_to_html('"');
	for (c_k = 0; c_dst[c_k]; c_k++)
		char_to_html(c_dst[c_k]);
	char_to_html('"');
}


void send_snmp(void) __banked
{
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	str_code_to_html("{\"enabled\":");
	bool_to_html(snmp_cfg.on);
	str_code_to_html(",\"community\":");
	c_dst = snmp_cfg.community;
	j_text();
	str_code_to_html(",\"location\":");
	c_dst = snmp_cfg.location;
	j_text();
	str_code_to_html(",\"contact\":");
	c_dst = snmp_cfg.contact;
	j_text();
	char_to_html('}');
}
