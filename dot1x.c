/*
 * 802.1X authenticator, port based: one authenticated device opens the port.
 *
 * The switch asks a device on an "auto" port for its identity with EAP
 * Request/Identity over EAPOL, and relays the EAP conversation that follows to
 * a RADIUS server in Access-Request / Access-Challenge exchanges (RFC 2865,
 * RFC 3579) until the server accepts or rejects. Until then the port carries
 * only EAPOL: port isolation fences it off (port_isolation_gate()) and
 * dot1x_rx() drops the rest of what it sends to the CPU. A device that never
 * answers can be let in by its MAC address instead (MAB): after
 * DOT1X_MAB_AFTER unanswered requests, the MAC of a frame seen on the port
 * goes to the server as user name and password.
 *
 * The state machine follows 802.1X-2004's authenticator PAE and backend
 * machines, collapsed into one per port: CONNECTING sends Request/Identity
 * every DOT1X_TX_PERIOD s, AUTHENTICATING relays, and a failure holds the port
 * for DOT1X_QUIET_PERIOD s. Re-authentication keeps the port open until it
 * fails.
 *
 * Frames are only sent where uip_buf is free to take them: dot1x_rx(), the
 * RADIUS callback and dot1x_second(). Commands, which may run inside an HTTP
 * request, only set timers. RADIUS requests are built in their own buffers and
 * leave from the UDP poll. The functions are not reentrant and pass their
 * operands in XDATA (doc/internal-ram.md): d_port/d_p is the port at hand.
 */

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_trap.h"
#include "machine.h"
#include "uip/uip.h"
#include "md5.h"
#include "dot1x.h"

#pragma codeseg BANK4
#pragma constseg BANK4

/* EAPOL packet types and EAP codes */
#define EAPOL_TYPE_HI	0x88
#define EAPOL_TYPE_LO	0x8e
#define EAPOL_EAP	0
#define EAPOL_START	1
#define EAPOL_LOGOFF	2
#define EAP_REQUEST	1
#define EAP_RESPONSE	2
#define EAP_SUCCESS	3
#define EAP_FAILURE	4
#define EAP_IDENTITY	1

/* RADIUS codes and attributes */
#define RAD_ACCESS_REQUEST	1
#define RAD_ACCESS_ACCEPT	2
#define RAD_ACCESS_REJECT	3
#define RAD_ACCESS_CHALLENGE	11
#define A_USER_NAME		1
#define A_USER_PASSWORD		2
#define A_NAS_IP		4
#define A_NAS_PORT		5
#define A_SERVICE_TYPE		6
#define A_FRAMED_MTU		12
#define A_STATE			24
#define A_SESSION_TIMEOUT	27
#define A_TERMINATION_ACTION	29
#define A_CALLED_STATION	30
#define A_CALLING_STATION	31
#define A_NAS_PORT_TYPE		61
#define A_EAP_MESSAGE		79
#define A_MESSAGE_AUTH		80

/* A UDP payload uIP can send and receive, it has no IP fragments. The server is
 * asked to keep its EAP packets well below that. */
#define RAD_MAX		1472
#define EAP_MAX		1472
#define FRAMED_MTU	1000
#define RAD_SLOTS	4

/* 01:80:c2:00:00:03: the fourth reserved-multicast register, after 00-02 */
#define RMA03_CONF	(RTL837X_RMA0_CONF + 3 * 4)

#define EAPOL_O		(&uip_buf[RTL_FRAME_DESC_SIZE])
#define EAPOL_O_HDR	26	/* addresses, CPU tag, type, EAPOL header */

struct rad_slot {
	uint8_t port;		/* DOT1X_PORTS: free */
	uint8_t send;
	uint8_t tries;
	uint16_t len;
	uint8_t buf[RAD_MAX];	/* kept for retransmission; the authenticator is at 4 */
};

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint16_t management_vlan;

__xdata uint8_t dot1x_on;
__xdata struct dot1x_config dot1x_cfg;
__xdata struct dot1x_port dot1x_ports[DOT1X_PORTS];
__xdata uint16_t dot1x_blocked;

static __xdata struct rad_slot rad_slots[RAD_SLOTS];
static __xdata uint8_t rad_next_id;
static __xdata struct uip_udp_conn * __xdata rad_conn;
static __xdata uint8_t rad_conn_ip[4];
static __xdata uint16_t rad_conn_port;
static __xdata uip_ipaddr_t rad_ip;

static __xdata uint8_t d_port;
static __xdata struct dot1x_port * __xdata d_p;
static __xdata uint8_t d_eap[EAP_MAX];		/* an EAP packet to the supplicant */
static __xdata uint16_t d_eap_len;
static __xdata uint8_t * __xdata d_src;		/* source MAC of a received frame */
static __xdata uint8_t * __xdata d_in;		/* EAP packet of the supplicant, or 0 (MAB) */
static __xdata uint16_t d_in_len;
static __xdata uint8_t * __xdata d_r;		/* RADIUS packet at hand */
static __xdata uint16_t d_n, d_at;
static __xdata uint8_t d_ovf, d_c;
static __xdata struct rad_slot * __xdata d_s;
static __xdata uint16_t d_secret_len;

static __code const char hex_lc[] = "0123456789abcdef";


static void d_log(__code const char *msg)
{
	print_string("802.1X: ");
	print_string(msg);
	print_string(" on port ");
	print_phys_port(d_port);
	write_char('\n');
}


static void d_select(uint8_t port)
{
	d_port = port;
	d_p = &dot1x_ports[port];
}


static uint8_t d_controlled(void)
{
	return dot1x_on && dot1x_cfg.mode[d_port] == DOT1X_AUTO;
}


/* Recompute the fenced-off ports; newly closed ones lose their learned addresses */
static void d_gate(void)
{
	static __xdata uint16_t b, was;
	static __xdata uint8_t i;

	b = 0;
	for (i = MACHINE_MIN_PORT; dot1x_on && i <= MACHINE_MAX_PORT; i++)
		if (dot1x_cfg.mode[i] == DOT1X_FORCE_UNAUTH
		    || (dot1x_cfg.mode[i] == DOT1X_AUTO && !dot1x_ports[i].authorized))
			b |= PORT_BIT(i);
	if (b == dot1x_blocked)
		return;
	was = dot1x_blocked;
	dot1x_blocked = b;
	port_isolation_gate(b);
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++)
		if ((b & ~was) & PORT_BIT(i))
			port_l2_forget_port(i);
}


static void d_authorize(uint8_t on)
{
	d_p->authorized = on;
	d_gate();
}


/* ---- to the supplicant ---- */

static void eap_tx(void)
{
	static __xdata uint16_t vlan, n;

	if (d_eap_len > EAP_MAX)
		return;
	memcpyc(EAPOL_O, (__code const uint8_t *)"\x01\x80\xc2\x00\x00\x03", 6);
	memcpy(EAPOL_O + 6, uip_ethaddr.addr, 6);
	EAPOL_O[12] = RTL_FRAME_TAG_ID >> 8;
	EAPOL_O[13] = RTL_FRAME_TAG_ID & 0xff;
	EAPOL_O[14] = RTL_FRAME_TAG_VERSION;
	EAPOL_O[15] = 0;
	EAPOL_O[16] = 0;
	EAPOL_O[17] = RTL_TAG_LEARN_DIS | RTL_TAG_KEEP;
	n = PORT_BIT(d_port);
	EAPOL_O[18] = n >> 8;
	EAPOL_O[19] = n;
	EAPOL_O[20] = EAPOL_TYPE_HI;
	EAPOL_O[21] = EAPOL_TYPE_LO;
	EAPOL_O[22] = 2;		/* 802.1X-2004 */
	EAPOL_O[23] = EAPOL_EAP;
	EAPOL_O[24] = d_eap_len >> 8;
	EAPOL_O[25] = d_eap_len;
	memcpy(EAPOL_O + EAPOL_O_HDR, d_eap, d_eap_len);
	n = EAPOL_O_HDR + d_eap_len;
	/* the shortest Ethernet frame, behind the CPU tag */
	while (n < 60 + RTL_TAG_SIZE)
		EAPOL_O[n++] = 0;
	vlan = management_vlan;
	management_vlan = 0;
	uip_len = n;
	tcpip_output();
	management_vlan = vlan;
	uip_len = 0;
}


static void eap_simple(uint8_t code)
{
	d_eap[0] = code;
	d_eap[1] = d_p->last_id;
	d_eap[2] = 0;
	d_eap[3] = 4;
	d_eap_len = 4;
	eap_tx();
}


static void request_identity(void)
{
	d_p->last_id = ++d_p->eap_id;
	d_eap[0] = EAP_REQUEST;
	d_eap[1] = d_p->eap_id;
	d_eap[2] = 0;
	d_eap[3] = 5;
	d_eap[4] = EAP_IDENTITY;
	d_eap_len = 5;
	eap_tx();
}


static void cancel_server(void)
{
	if (d_p->slot != DOT1X_NO_SLOT) {
		rad_slots[d_p->slot].port = DOT1X_PORTS;
		d_p->slot = DOT1X_NO_SLOT;
	}
}


/* Ask for an identity; the port stays open if it was (re-authentication) */
static void connecting(void)
{
	cancel_server();
	d_p->state = DOT1X_S_CONNECTING;
	d_p->state_len = 0;
	request_identity();
	d_p->tx_count = 1;
	d_p->timer = DOT1X_TX_PERIOD;
}


static void failed(void)
{
	cancel_server();
	if (d_p->fail != 0xffff)
		d_p->fail++;
	d_p->state = DOT1X_S_HELD;
	d_p->timer = DOT1X_QUIET_PERIOD;
	d_p->state_len = 0;
	d_authorize(0);
	d_log("authentication failed");
}


static void failed_tell(void)
{
	if (d_p->run == DOT1X_M_EAP)
		eap_simple(EAP_FAILURE);
	failed();
}


/* ---- RADIUS ---- */

static void pb(uint8_t b)
{
	d_c = b;
	if (d_n < RAD_MAX)
		d_r[d_n++] = d_c;
	else
		d_ovf = 1;
}


static void attr(uint8_t type)
{
	d_at = d_n + 1;
	pb(type);
	pb(0);
}


static void attr_end(void)
{
	if (!d_ovf)
		d_r[d_at] = d_n - d_at + 1;
}


static void attr_small(uint8_t type)
{
	static __xdata uint8_t v;

	v = d_c;
	attr(type);
	pb(0);
	pb(0);
	pb(0);
	pb(v);
	attr_end();
}
#define ATTR_SMALL(t, v)	do { d_c = (v); attr_small(t); } while (0)


static __xdata uint8_t * __xdata d_mac;

static void attr_mac(uint8_t type)
{
	static __xdata uint8_t i, v;

	attr(type);
	for (i = 0; i < 6; i++) {
		if (i)
			pb('-');
		v = d_mac[i];
		pb(hex_lc[v >> 4] & (v >= 0xa0 ? 0xdf : 0xff));
		pb(hex_lc[v & 0xf] & ((v & 0xf) >= 10 ? 0xdf : 0xff));
	}
	attr_end();
}


/* RFC 2865 5.2, for MAB only: the 12-digit identity hidden with the secret */
static void attr_password(void)
{
	static __xdata uint8_t i;

	md5_init();
	MD5_ADD(dot1x_cfg.secret, d_secret_len);
	MD5_ADD(d_r + 4, 16);
	md5_final();
	attr(A_USER_PASSWORD);
	for (i = 0; i < 16; i++)
		pb((i < 12 ? d_p->identity[i] : 0) ^ md5_out[i]);
	attr_end();
}


/* Build the Access-Request of d_p, with the EAP packet d_in or for MAB; it leaves from the poll */
static void access_request(void)
{
	static __xdata uint16_t i;
	static __xdata uint8_t k;

	cancel_server();
	for (k = 0; k < RAD_SLOTS; k++)
		if (rad_slots[k].port == DOT1X_PORTS)
			break;
	if (k == RAD_SLOTS) {
		/* all in use: come back shortly */
		d_p->timer = 3;
		if (d_p->run == DOT1X_M_MAB && d_p->authorized) {
			d_p->state = DOT1X_S_AUTHENTICATED;
			d_p->reauth_left = 3;
			return;
		}
		d_p->state = DOT1X_S_CONNECTING;
		d_p->tx_count = d_p->run == DOT1X_M_MAB ? DOT1X_MAB_AFTER : 0;
		return;
	}
	d_s = &rad_slots[k];
	d_s->port = d_port;
	d_s->tries = 1;
	d_p->slot = k;
	d_p->state = DOT1X_S_AUTHENTICATING;
	d_p->timer = DOT1X_SERVER_FIRST;
	d_secret_len = strlen_x(dot1x_cfg.secret);

	d_r = d_s->buf;
	d_r[0] = RAD_ACCESS_REQUEST;
	d_r[1] = rad_next_id++;
	d_n = 4;
	d_ovf = 0;
	for (k = 0; k < 16; k++) {
		if (!(k & 3))
			get_random_32();
		pb(sfr_data[k & 3]);
	}
	attr(A_USER_NAME);
	for (k = 0; d_p->identity[k]; k++)
		pb(d_p->identity[k]);
	if (!k) {
		pb('?');
	}
	attr_end();
	if (!d_in)
		attr_password();
	attr(A_NAS_IP);
	for (k = 0; k < 4; k++)
		pb(((__xdata uint8_t *)uip_hostaddr)[k]);
	attr_end();
	ATTR_SMALL(A_NAS_PORT, machine.log_to_phys_port[d_port]);
	ATTR_SMALL(A_SERVICE_TYPE, d_in ? 2 : 10);	/* Framed, or Call-Check for MAB */
	attr(A_FRAMED_MTU);
	pb(0);
	pb(0);
	pb(FRAMED_MTU >> 8);
	pb(FRAMED_MTU & 0xff);
	attr_end();
	d_mac = uip_ethaddr.addr;
	attr_mac(A_CALLED_STATION);
	if (d_p->mac_valid) {
		d_mac = d_p->mac;
		attr_mac(A_CALLING_STATION);
	}
	ATTR_SMALL(A_NAS_PORT_TYPE, 15);		/* Ethernet */
	if (d_p->state_len) {
		attr(A_STATE);
		for (k = 0; k < d_p->state_len; k++)
			pb(d_p->state_attr[k]);
		attr_end();
	}
	if (d_in) {
		for (i = 0; i < d_in_len; i++) {
			if (!(i % 253U)) {
				if (i)
					attr_end();
				attr(A_EAP_MESSAGE);
			}
			pb(d_in[i]);
		}
		attr_end();
	}
	attr(A_MESSAGE_AUTH);
	for (k = 0; k < 16; k++)
		pb(0);
	attr_end();
	if (d_ovf) {
		d_log("EAP packet too long for RADIUS");
		failed_tell();
		return;
	}
	d_r[2] = d_n >> 8;
	d_r[3] = d_n;
	d_s->len = d_n;
	md5_p = (__xdata uint8_t *)dot1x_cfg.secret;
	md5_n = d_secret_len;
	hmac_md5_init();
	MD5_ADD(d_r, d_n);
	hmac_md5_final();
	memcpy(d_r + d_n - 16, md5_out, 16);
	d_s->send = 1;
}


/* Response Authenticator and Message-Authenticator (at d_at, 0: none) of the d_n bytes at d_r */
static uint8_t rad_verify(void)
{
	static __xdata uint8_t k, bad;
	static __xdata uint8_t got[16];

	d_secret_len = strlen_x(dot1x_cfg.secret);
	md5_init();
	MD5_ADD(d_r, 4);
	MD5_ADD(d_s->buf + 4, 16);
	MD5_ADD(d_r + 20, d_n - 20);
	MD5_ADD(dot1x_cfg.secret, d_secret_len);
	md5_final();
	bad = 0;
	for (k = 0; k < 16; k++)
		bad |= md5_out[k] ^ d_r[4 + k];
	if (bad)
		return 0;
	if (!d_at)
		return !d_eap_len && d_p->run != DOT1X_M_EAP;
	/* RFC 3579 3.2: with the Request Authenticator, and the attribute zeroed */
	for (k = 0; k < 16; k++) {
		got[k] = d_r[d_at + k];
		d_r[d_at + k] = 0;
	}
	md5_p = (__xdata uint8_t *)dot1x_cfg.secret;
	md5_n = d_secret_len;
	hmac_md5_init();
	MD5_ADD(d_r, 4);
	MD5_ADD(d_s->buf + 4, 16);
	MD5_ADD(d_r + 20, d_n - 20);
	hmac_md5_final();
	for (k = 0; k < 16; k++)
		bad |= md5_out[k] ^ got[k];
	return !bad;
}


static __xdata uint8_t * __xdata g_p;
static __xdata uint32_t g_v;

static void get32(void)
{
	((__xdata uint8_t *)&g_v)[0] = g_p[3];
	((__xdata uint8_t *)&g_v)[1] = g_p[2];
	((__xdata uint8_t *)&g_v)[2] = g_p[1];
	((__xdata uint8_t *)&g_v)[3] = g_p[0];
}


/* A RADIUS packet of d_n bytes at d_r, in uip_buf */
static void radius_rx(void) __reentrant
{
	static __xdata uint16_t i, state_at;
	static __xdata uint8_t t, l, code, state_len, radius_reauth;
	static __xdata uint32_t session;

	if (d_n < 20)
		return;
	i = ((uint16_t)d_r[2] << 8) | d_r[3];
	if (i < 20 || i > d_n)
		return;
	d_n = i;
	for (l = 0; l < RAD_SLOTS; l++) {
		d_s = &rad_slots[l];
		if (d_s->port < DOT1X_PORTS && d_s->buf[1] == d_r[1])
			break;
	}
	if (l == RAD_SLOTS)
		return;
	d_select(d_s->port);

	d_eap_len = 0;
	d_at = 0;
	state_len = 0;
	radius_reauth = 0;
	session = 0;
	for (i = 20; i < d_n; i += l) {
		if (i + 2 > d_n)
			return;
		t = d_r[i];
		l = d_r[i + 1];
		if (l < 2 || i + l > d_n)
			return;
		g_p = d_r + i + 2;
		if (t == A_EAP_MESSAGE && d_eap_len + l - 2 <= EAP_MAX) {
			memcpy(d_eap + d_eap_len, g_p, l - 2);
			d_eap_len += l - 2;
		} else if (t == A_MESSAGE_AUTH && l == 18) {
			d_at = i + 2;
		} else if (t == A_STATE && l - 2 <= DOT1X_STATE_MAX) {
			state_at = i + 2;
			state_len = l - 2;
		} else if (t == A_SESSION_TIMEOUT && l == 6) {
			get32();
			session = g_v;
		} else if (t == A_TERMINATION_ACTION && l == 6) {
			get32();
			radius_reauth = g_v == 1;
		}
	}
	if (!rad_verify()) {
		d_log("RADIUS reply fails its authenticator (wrong secret?)");
		return;
	}
	code = d_r[0];
	cancel_server();
	for (t = 0; t < state_len; t++)
		d_p->state_attr[t] = d_r[state_at + t];
	d_p->state_len = state_len;
	if (d_eap_len < 4)
		d_eap_len = 0;

	if (code == RAD_ACCESS_CHALLENGE) {
		if (d_p->run != DOT1X_M_EAP || !d_eap_len) {
			failed_tell();
			return;
		}
		d_p->last_id = d_eap[1];
		d_p->timer = DOT1X_SUPP_TIMEOUT;
		eap_tx();
	} else if (code == RAD_ACCESS_ACCEPT) {
		if (d_p->run == DOT1X_M_EAP) {
			if (d_eap_len)
				eap_tx();
			else
				eap_simple(EAP_SUCCESS);
		}
		d_p->method = d_p->run;
		if (d_p->ok != 0xffff)
			d_p->ok++;
		d_p->state = DOT1X_S_AUTHENTICATED;
		d_p->state_len = 0;
		if (!session || (!radius_reauth && dot1x_cfg.reauth_s))
			session = dot1x_cfg.reauth_s;
		d_p->reauth_left = session;
		d_authorize(1);
		d_log(d_p->run == DOT1X_M_MAB ? "MAC authenticated" : "authenticated");
	} else {
		if (d_p->run == DOT1X_M_EAP && d_eap_len) {
			eap_tx();
			failed();
		} else {
			failed_tell();
		}
	}
}


void dot1x_callback(uint16_t lport) __banked
{
	static __xdata uint8_t k;

	if (!rad_conn || lport != rad_conn->lport)
		return;
	if (uip_newdata()) {
		d_r = (__xdata uint8_t *)uip_appdata;
		d_n = uip_datalen();
		radius_rx();
		return;
	}
	for (k = 0; k < RAD_SLOTS; k++) {
		if (rad_slots[k].port < DOT1X_PORTS && rad_slots[k].send) {
			rad_slots[k].send = 0;
			uip_send(rad_slots[k].buf, rad_slots[k].len);
			return;
		}
	}
}


/* ---- EAPOL from the supplicant ---- */

static void start_mab(void)
{
	static __xdata uint8_t i;

	for (i = 0; i < 6; i++) {
		d_p->identity[2 * i] = hex_lc[d_p->mac[i] >> 4];
		d_p->identity[2 * i + 1] = hex_lc[d_p->mac[i] & 0xf];
	}
	d_p->identity[12] = 0;
	d_p->run = DOT1X_M_MAB;
	d_p->state_len = 0;
	d_in = 0;
	access_request();
}


static void note_mac(void)
{
	memcpy(d_p->mac, d_src, 6);
	d_p->mac_valid = 1;
}


/* The EAPOL PDU of d_in_len bytes at d_in */
static void eapol_rx(void) __reentrant
{
	static __xdata uint16_t n;
	static __xdata uint8_t type;

	if (!d_controlled() || d_in_len < 4)
		return;
	if (d_p->state == DOT1X_S_DOWN || d_p->state == DOT1X_S_HELD)
		return;
	n = ((uint16_t)d_in[2] << 8) | d_in[3];
	if (n > d_in_len - 4)
		return;
	type = d_in[1];
	if (type == EAPOL_START) {
		note_mac();
		connecting();
		return;
	}
	if (type == EAPOL_LOGOFF) {
		cancel_server();
		d_p->state = DOT1X_S_CONNECTING;
		d_p->state_len = 0;
		d_p->tx_count = 0;
		d_p->timer = DOT1X_TX_PERIOD;
		d_authorize(0);
		d_log("logoff");
		return;
	}
	if (type != EAPOL_EAP || n < 5)
		return;
	d_in += 4;
	d_in_len = ((uint16_t)d_in[2] << 8) | d_in[3];
	if (d_in[0] != EAP_RESPONSE || d_in_len < 5 || d_in_len > n)
		return;

	if (d_in[4] == EAP_IDENTITY && d_in[1] == d_p->eap_id && d_p->state == DOT1X_S_CONNECTING) {
		for (n = 0; n < DOT1X_IDENTITY_MAX - 1 && n + 5 < d_in_len; n++)
			d_p->identity[n] = d_in[5 + n];
		d_p->identity[n] = 0;
		note_mac();
		d_p->run = DOT1X_M_EAP;
		d_p->state_len = 0;
		access_request();
		return;
	}
	/* The rest of the conversation, while we wait for the supplicant */
	if (d_p->state == DOT1X_S_AUTHENTICATING && d_p->run == DOT1X_M_EAP
	    && d_p->slot == DOT1X_NO_SLOT && d_in[1] == d_p->last_id)
		access_request();
}


uint8_t dot1x_rx(void) __banked
{
	static __xdata uint8_t at;

	at = uip_buf[TRAP_RX_HDR_SIZE - 1] & 0x0f;
	if (!MACHINE_PORT_VALID(at))
		return 0;
	d_select(at);
	d_src = uip_buf + 6;
	at = TRAP_RX_BODY();
	if (uip_len > at + 2 && uip_buf[at] == EAPOL_TYPE_HI && uip_buf[at + 1] == EAPOL_TYPE_LO) {
		d_in = uip_buf + at + 2;
		d_in_len = uip_len - at - 2;
		eapol_rx();
		uip_len = 0;
		return 1;
	}
	if (!(dot1x_blocked & PORT_BIT(d_port)))
		return 0;
	/* a closed port: its MAC may be all MAB gets to see */
	if (d_controlled() && !(d_src[0] & 1) && d_p->state == DOT1X_S_CONNECTING && !d_p->mac_valid)
		note_mac();
	uip_len = 0;
	return 1;
}


/* ---- time, links and configuration ---- */

static void reset_port(uint8_t state)
{
	d_p->state = state;
	cancel_server();
	d_p->authorized = 0;
	d_p->mac_valid = 0;
	d_p->identity[0] = 0;
	d_p->reauth_left = 0;
	d_p->state_len = 0;
	d_p->tx_count = 0;
	d_p->timer = 0;
}


static void step(void)
{
	if (d_p->state == DOT1X_S_AUTHENTICATED) {
		if (d_p->reauth_left && !--d_p->reauth_left) {
			d_log("re-authentication");
			if (d_p->method == DOT1X_M_MAB) {
				start_mab();
			} else {
				connecting();
			}
		}
		return;
	}
	if (d_p->timer && --d_p->timer)
		return;
	if (d_p->state == DOT1X_S_CONNECTING) {
		if (d_p->tx_count >= DOT1X_MAB_AFTER && d_p->authorized) {
			/* re-authentication went unanswered */
			failed();
		} else if (d_p->tx_count >= DOT1X_MAB_AFTER && dot1x_cfg.mab[d_port] && d_p->mac_valid) {
			start_mab();
		} else {
			request_identity();
			if (d_p->tx_count < 255)
				d_p->tx_count++;
			d_p->timer = DOT1X_TX_PERIOD;
		}
	} else if (d_p->state == DOT1X_S_AUTHENTICATING) {
		if (d_p->slot == DOT1X_NO_SLOT) {
			/* the supplicant went quiet */
			connecting();
		} else if (rad_slots[d_p->slot].tries < DOT1X_SERVER_TRIES) {
			rad_slots[d_p->slot].tries++;
			rad_slots[d_p->slot].send = 1;
			d_p->timer = DOT1X_SERVER_TIMEOUT;
		} else {
			d_log("no answer from the RADIUS server");
			failed_tell();
		}
	} else if (d_p->state == DOT1X_S_HELD) {
		connecting();
	}
}


static void link_down(void)
{
	if (d_p->state != DOT1X_S_DOWN) {
		reset_port(DOT1X_S_DOWN);
		d_gate();
	}
}


void dot1x_second(void) __banked
{
	static __xdata uint8_t i;
	static __xdata uint16_t link;

	if (!dot1x_on)
		return;
	link = port_link_mask();
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		d_select(i);
		if (!d_controlled())
			continue;
		if (!(link & PORT_BIT(i))) {
			link_down();
			continue;
		}
		if (d_p->state == DOT1X_S_DOWN) {
			/* the first request a second after the link */
			reset_port(DOT1X_S_CONNECTING);
			d_p->timer = 2;
		}
		step();
	}
}


/* A link that changed, even if it is back by now, may have another device on it */
void dot1x_link_change(uint16_t changed) __banked
{
	static __xdata uint8_t i;
	static __xdata uint16_t m;

	m = changed;
	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		d_select(i);
		if ((m & PORT_BIT(i)) && d_controlled())
			link_down();
	}
}


void dot1x_restart(uint8_t port) __banked
{
	d_select(port);
	if (d_controlled() && d_p->state != DOT1X_S_DOWN) {
		reset_port(DOT1X_S_CONNECTING);
		d_p->timer = 1;
		d_gate();
	}
}


static void radius_connect(void)
{
	static __xdata uint8_t same, k;

	same = dot1x_on && rad_conn && rad_conn_port == dot1x_cfg.radius_port;
	for (k = 0; k < 4; k++)
		if (rad_conn_ip[k] != dot1x_cfg.radius_ip[k])
			same = 0;
	if (same)
		return;
	if (rad_conn) {
		uip_udp_remove(rad_conn);
		rad_conn = 0;
	}
	if (!dot1x_on || !dot1x_cfg.radius_ip[0])
		return;
	uip_ipaddr(rad_ip, dot1x_cfg.radius_ip[0], dot1x_cfg.radius_ip[1],
		   dot1x_cfg.radius_ip[2], dot1x_cfg.radius_ip[3]);
	rad_conn = uip_udp_new(&rad_ip, HTONS(dot1x_cfg.radius_port));
	if (!rad_conn)
		print_string("802.1X: no free UDP connection\n");
	for (k = 0; k < 4; k++)
		rad_conn_ip[k] = dot1x_cfg.radius_ip[k];
	rad_conn_port = dot1x_cfg.radius_port;
}


void dot1x_apply(void) __banked
{
	static __xdata uint8_t i, want;

	for (i = MACHINE_MIN_PORT; i <= MACHINE_MAX_PORT; i++) {
		d_select(i);
		if (!dot1x_on || dot1x_cfg.mode[i] == DOT1X_FORCE_AUTH)
			want = DOT1X_S_OFF;
		else if (dot1x_cfg.mode[i] == DOT1X_FORCE_UNAUTH)
			want = DOT1X_S_BLOCKED;
		else
			want = DOT1X_S_DOWN;
		if (want == DOT1X_S_DOWN) {
			/* an auto port waits for dot1x_second() to see its link */
			if (d_p->state == DOT1X_S_OFF || d_p->state == DOT1X_S_BLOCKED)
				reset_port(DOT1X_S_DOWN);
		} else if (d_p->state != want) {
			reset_port(want);
		}
	}
	radius_connect();
	trap_rma_set(RMA03_CONF, dot1x_on ? RTL837X_RMA_ACT_TRAP : RTL837X_RMA_ACT_FORWARD);
	d_gate();
}


void dot1x_init(void) __banked
{
	static __xdata uint8_t i;

	dot1x_on = 0;
	dot1x_blocked = 0;
	rad_conn = 0;
	rad_conn_ip[0] = 0;
	dot1x_cfg.radius_ip[0] = dot1x_cfg.radius_ip[1] = dot1x_cfg.radius_ip[2] = dot1x_cfg.radius_ip[3] = 0;
	dot1x_cfg.radius_port = 1812;
	dot1x_cfg.secret[0] = 0;
	dot1x_cfg.reauth_s = 0;
	for (i = 0; i < RAD_SLOTS; i++)
		rad_slots[i].port = DOT1X_PORTS;
	/* RADIUS ids start anywhere, so a restart does not reuse the last ones */
	get_random_32();
	rad_next_id = sfr_data[0];
	for (i = 0; i < DOT1X_PORTS; i++) {
		d_select(i);
		d_p->slot = DOT1X_NO_SLOT;
		d_p->eap_id = sfr_data[1];
		d_p->ok = 0;
		d_p->fail = 0;
		d_p->method = DOT1X_M_NONE;
		d_p->run = DOT1X_M_NONE;
		reset_port(DOT1X_S_OFF);
		dot1x_cfg.mode[i] = DOT1X_FORCE_AUTH;
		dot1x_cfg.mab[i] = 0;
	}
}
