/*
 * dot1x.c against a simulated supplicant (EAP-MD5) and a simulated RADIUS
 * server that checks every Request's Message-Authenticator and signs every
 * reply as RFC 2865/3579 say. EAPOL crosses in uip_buf as the ASIC delivers
 * and takes it, RADIUS through the uIP callback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl837x_common.h"
#include "machine.h"
#include "uip.h"
#include "rtl837x_regs.h"
#include "md5.h"
#include "dot1x.h"

static int failures, checks;
#define check(c, ...) do { checks++; if (c) { printf("PASS  "); } else { printf("FAIL  "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); } while (0)

/* ---- the platform ---- */
uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
void *uip_appdata;
uint8_t uip_flags;
uip_ipaddr_t uip_hostaddr;
struct uip_eth_addr uip_ethaddr = { .addr = { 0x06, 0x0f, 0x00, 0xdd, 0x3b, 0x3e } };
const struct machine machine = { .log_to_phys_port = { 1, 2, 3, 1, 2, 3, 4, 5, 6 },
	.phys_to_log_port = { 3, 4, 5, 6, 7, 8 } };
struct machine_runtime machine_detected;
uint16_t management_vlan = 5;
uint8_t sfr_data[4];

static uint16_t gate_mask = 0xffff, links_up, forgotten;
static uint8_t rma_act = 0xff;
static unsigned rnd = 1;

void port_isolation_gate(uint16_t m) { gate_mask = m; }
void port_l2_forget_port(uint8_t p) { forgotten |= 1 << p; }
uint16_t port_link_mask(void) { return links_up; }
void trap_rma_set(uint16_t reg, uint8_t act) { if (reg == 0x4ed8) rma_act = act; }
void get_random_32(void) { for (int i = 0; i < 4; i++) sfr_data[i] = (rnd = rnd * 1103515245 + 12345) >> 16; }
void print_string(const char *s) { (void)s; }
void print_phys_port(uint8_t p) { (void)p; }
void write_char(char c) { (void)c; }
void memcpyc(uint8_t *d, const uint8_t *s, uint16_t n) { memcpy(d, s, n); }
uint16_t strlen_x(const char *s) { return strlen(s); }
static struct uip_udp_conn udp_conn;
static int udp_news;
struct uip_udp_conn *uip_udp_new(uip_ipaddr_t *ip, uint16_t rport)
{
	memcpy(udp_conn.ripaddr, ip, 4);
	udp_conn.rport = rport;
	udp_conn.lport = HTONS(4097);
	udp_news++;
	return &udp_conn;
}

/* the checks' own use of the digests */
static void dot1x_md5(const uint8_t *a, uint16_t na, const uint8_t *b, uint16_t nb, uint8_t out[16])
{
	md5_init();
	MD5_ADD(a, na);
	MD5_ADD(b, nb);
	md5_final();
	memcpy(out, md5_out, 16);
}

static void dot1x_hmac_md5(const uint8_t *key, uint16_t nkey, const uint8_t *m, uint16_t nm, uint8_t out[16])
{
	md5_p = (uint8_t *)key;
	md5_n = nkey;
	hmac_md5_init();
	MD5_ADD(m, nm);
	hmac_md5_final();
	memcpy(out, md5_out, 16);
}

/* Messages in flight: to the supplicant on a port, and to the server */
#define QN 64
struct msg { int port; uint16_t len; uint8_t d[1800]; };
static struct msg q_supp[QN], q_srv[QN];
static int n_supp, n_srv;
static int radius_sent;
static bool server_down;

static int bad_frames;

void tcpip_output(void)
{
	const uint8_t *f = uip_buf + RTL_FRAME_DESC_SIZE;
	uint16_t pmask = f[18] << 8 | f[19], len = f[24] << 8 | f[25];
	int port = __builtin_ctz(pmask);

	if (memcmp(f, "\x01\x80\xc2\0\0\x03", 6) || memcmp(f + 6, uip_ethaddr.addr, 6) || f[12] != 0x88 || f[13] != 0x99
	    || (pmask & (pmask - 1)) || f[20] != 0x88 || f[21] != 0x8e || f[22] != 2 || management_vlan
	    || uip_len != (26 + len < 68 ? 68 : 26 + len))
		bad_frames++;
	if (n_supp < QN) {
		q_supp[n_supp].port = port;
		q_supp[n_supp].len = len + 4;
		memcpy(q_supp[n_supp++].d, f + 22, len + 4);
	}
}

static uint16_t poll_len;
void uip_send(const void *p, uint16_t len)
{
	poll_len = len;
	radius_sent++;
	if (server_down || n_srv >= QN)
		return;
	q_srv[n_srv].len = len;
	memcpy(q_srv[n_srv++].d, p, len);
}

/* the UDP poll of the main loop, until nothing is left to send */
static void poll_udp(void)
{
	do {
		poll_len = 0;
		uip_flags = UIP_POLL;
		dot1x_callback(udp_conn.lport);
	} while (poll_len);
}

static void radius_rx(const uint8_t *p, uint16_t len)
{
	uip_appdata = uip_buf + UIP_LLH_LEN + UIP_IPUDPH_LEN;
	memcpy(uip_appdata, p, len);
	uip_len = len;
	uip_flags = UIP_NEWDATA;
	dot1x_callback(udp_conn.lport);
}

/* a frame from a port, as the CPU gets it: CPU tag, 802.1Q tag; returns whether 802.1X took it */
static int frame_rx(int port, const uint8_t *src, uint16_t type, const uint8_t *body, uint16_t len)
{
	memset(uip_buf, 0xee, sizeof(uip_buf));
	memcpy(uip_buf, "\x01\x80\xc2\0\0\x03", 6);
	memcpy(uip_buf + 6, src, 6);
	memcpy(uip_buf + 12, "\x88\x99\x04\0\0\0\0", 7);
	uip_buf[19] = port;
	memcpy(uip_buf + 20, "\x81\0\0\x01", 4);
	uip_buf[24] = type >> 8;
	uip_buf[25] = type;
	memcpy(uip_buf + 26, body, len);
	uip_len = 26 + len;
	return dot1x_rx();
}
#define dot1x_radius_rx(p, n)		radius_rx(p, n)
#define dot1x_eapol_rx(p, s, f, n)	frame_rx(p, s, 0x888e, f, n)
#define dot1x_seen(p, s)		frame_rx(p, s, 0x0800, (const uint8_t *)"x", 1)
static void link_(int port, bool up)
{
	if (up) links_up |= 1 << port; else links_up &= ~(1 << port);
	dot1x_link_change(1 << port);
}

/* ---- the supplicant ---- */
struct supp {
	bool present;			/* answers EAPOL at all */
	const char *user, *password;
	uint8_t mac[6];
	int successes, failures, id_requests, big_seen;
};
static struct supp supp[10];

static void supp_send_eap(int port, const uint8_t *eap, uint16_t len)
{
	uint8_t f[1800];
	f[0] = 2; f[1] = 0; f[2] = len >> 8; f[3] = len;
	memcpy(f + 4, eap, len);
	dot1x_eapol_rx(port, supp[port].mac, f, len + 4);
}

static void supp_rx(int port, const uint8_t *pdu, uint16_t len)
{
	struct supp *s = &supp[port];
	const uint8_t *e = pdu + 4;
	uint8_t r[1600];

	if (!s->present || pdu[1] != 0 || len < 8)
		return;
	if (e[0] == 3) { s->successes++; return; }
	if (e[0] == 4) { s->failures++; return; }
	if (e[0] != 1)
		return;
	r[0] = 2; r[1] = e[1];
	if (e[4] == 1) {			/* Identity */
		s->id_requests++;
		uint16_t n = strlen(s->user);
		r[4] = 1; memcpy(r + 5, s->user, n);
		n += 5; r[2] = n >> 8; r[3] = n;
		supp_send_eap(port, r, n);
	} else if (e[4] == 4) {			/* MD5-Challenge: MD5(id || password || challenge) */
		uint8_t buf[200], h[16];
		uint16_t vs = e[5];
		buf[0] = e[1];
		memcpy(buf + 1, s->password, strlen(s->password));
		memcpy(buf + 1 + strlen(s->password), e + 6, vs);
		dot1x_md5(buf, 1 + strlen(s->password) + vs, NULL, 0, h);
		r[4] = 4; r[5] = 16; memcpy(r + 6, h, 16);
		r[2] = 0; r[3] = 22;
		supp_send_eap(port, r, 22);
	} else if (e[4] == 13) {		/* a big request: answer with a big response */
		uint16_t elen = e[2] << 8 | e[3];
		s->big_seen = elen;
		uint16_t n = 700;
		memset(r, 0x5a, n);
		r[0] = 2; r[1] = e[1]; r[2] = n >> 8; r[3] = n; r[4] = 13;
		supp_send_eap(port, r, n);
	}
}

/* ---- the RADIUS server ---- */
static const char *srv_secret = "testing123";
static const char *nas_secret = "testing123";
static int srv_requests, srv_bad_ma, srv_mab_ok, srv_big;
static uint32_t last_nas_port;
static uint8_t challenges[10][16];	/* by NAS-Port */
#define challenge challenges[last_nas_port % 10]
static uint32_t session_timeout;
static bool srv_big_mode;
static char last_calling[32];
static uint32_t last_service;

static int attr_find(const uint8_t *r, int len, int type, int from)
{
	for (int i = from; i + 2 <= len; i += r[i + 1]) {
		if (r[i + 1] < 2)
			return -1;
		if (r[i] == type)
			return i;
	}
	return -1;
}

static void srv_reply(const uint8_t *req, uint8_t code, const uint8_t *eap, uint16_t elen, bool with_state)
{
	uint8_t r[1800], h[16];
	uint16_t n = 20;
	int ma_at;

	r[0] = code; r[1] = req[1];
	memcpy(r + 4, req + 4, 16);	/* the Request Authenticator, for MA and the hash */
	for (uint16_t i = 0; eap && i < elen; i += 253) {
		uint16_t k = elen - i > 253 ? 253 : elen - i;
		r[n] = 79; r[n + 1] = k + 2; memcpy(r + n + 2, eap + i, k); n += k + 2;
	}
	if (with_state) { r[n] = 24; r[n + 1] = 6; memcpy(r + n + 2, "st-1", 4); n += 6; }
	if (session_timeout && code == 2) {
		r[n] = 27; r[n + 1] = 6;
		r[n + 2] = session_timeout >> 24; r[n + 3] = session_timeout >> 16; r[n + 4] = session_timeout >> 8; r[n + 5] = session_timeout;
		n += 6;
		r[n] = 29; r[n + 1] = 6; r[n + 2] = 0; r[n + 3] = 0; r[n + 4] = 0; r[n + 5] = 1; n += 6;
	}
	ma_at = n + 2;
	r[n] = 80; r[n + 1] = 18; memset(r + n + 2, 0, 16); n += 18;
	r[2] = n >> 8; r[3] = n;
	dot1x_hmac_md5((const uint8_t *)srv_secret, strlen(srv_secret), r, n, h);
	memcpy(r + ma_at, h, 16);
	/* Response Authenticator: MD5(Code+ID+Length+RequestAuth+Attributes+Secret) */
	uint8_t tmp[1900];
	memcpy(tmp, r, n);
	memcpy(tmp + n, srv_secret, strlen(srv_secret));
	dot1x_md5(tmp, n + strlen(srv_secret), NULL, 0, h);
	memcpy(r + 4, h, 16);
	dot1x_radius_rx(r, n);
}

static void srv_rx(uint8_t *req, uint16_t len)
{
	uint8_t eap[1600], h[16], ma[16];
	uint16_t elen = 0;
	int i;

	srv_requests++;
	/* The Request's Message-Authenticator, with the attribute zeroed */
	i = attr_find(req, len, 80, 20);
	if (i < 0) { srv_bad_ma++; return; }
	memcpy(ma, req + i + 2, 16);
	memset(req + i + 2, 0, 16);
	dot1x_hmac_md5((const uint8_t *)srv_secret, strlen(srv_secret), req, len, h);
	memcpy(req + i + 2, ma, 16);
	if (memcmp(h, ma, 16)) { srv_bad_ma++; return; }

	i = attr_find(req, len, 31, 20);
	last_calling[0] = 0;
	if (i >= 0) { memcpy(last_calling, req + i + 2, req[i + 1] - 2); last_calling[req[i + 1] - 2] = 0; }
	i = attr_find(req, len, 5, 20);
	last_nas_port = i >= 0 ? (uint32_t)req[i + 5] : 0;
	i = attr_find(req, len, 6, 20);
	last_service = i >= 0 ? (uint32_t)req[i + 5] : 0;

	for (i = 20; (i = attr_find(req, len, 79, i)) >= 0; i += req[i + 1]) {
		memcpy(eap + elen, req + i + 2, req[i + 1] - 2);
		elen += req[i + 1] - 2;
	}
	if (!elen) {
		/* MAB: User-Password = User-Name, hidden with the secret */
		int u = attr_find(req, len, 1, 20), p = attr_find(req, len, 2, 20);
		char user[64], pw[64];
		memcpy(user, req + u + 2, req[u + 1] - 2); user[req[u + 1] - 2] = 0;
		uint8_t b[16], tmp[100];
		memcpy(tmp, srv_secret, strlen(srv_secret));
		memcpy(tmp + strlen(srv_secret), req + 4, 16);
		dot1x_md5(tmp, strlen(srv_secret) + 16, NULL, 0, b);
		for (int k = 0; k < 16; k++) pw[k] = req[p + 2 + k] ^ b[k];
		pw[16] = 0;
		bool ok = !strcmp(user, pw) && !strcmp(user, "020000000003");
		srv_mab_ok += ok;
		srv_reply(req, ok ? 2 : 3, NULL, 0, false);
		return;
	}
	if ((eap[2] << 8 | eap[3]) != elen) { srv_reply(req, 3, NULL, 0, false); return; }
	if (eap[4] == 1) {			/* Response/Identity: challenge it */
		uint8_t c[64];
		if (srv_big_mode) {
			uint16_t n = 900;
			memset(c, 0, sizeof(c));
			static uint8_t big[900];
			memset(big, 0xa5, n);
			big[0] = 1; big[1] = eap[1] + 1; big[2] = n >> 8; big[3] = n; big[4] = 13;
			srv_reply(req, 11, big, n, true);
			return;
		}
		for (int k = 0; k < 16; k++) challenge[k] = rand();
		c[0] = 1; c[1] = eap[1] + 1; c[2] = 0; c[3] = 22; c[4] = 4; c[5] = 16;
		memcpy(c + 6, challenge, 16);
		srv_reply(req, 11, c, 22, true);
		return;
	}
	if (attr_find(req, len, 24, 20) < 0) { srv_reply(req, 3, NULL, 0, false); return; }
	uint8_t res[4] = { 3, eap[1], 0, 4 }, rej[4] = { 4, eap[1], 0, 4 };
	if (eap[4] == 13) {
		srv_big = elen;
		srv_reply(req, 2, res, 4, false);
		return;
	}
	if (eap[4] == 4) {
		uint8_t buf[200], want[16];
		const char *pw = "secret";
		buf[0] = eap[1];
		memcpy(buf + 1, pw, strlen(pw));
		memcpy(buf + 1 + strlen(pw), challenge, 16);
		dot1x_md5(buf, 1 + strlen(pw) + 16, NULL, 0, want);
		bool ok = !memcmp(want, eap + 6, 16);
		srv_reply(req, ok ? 2 : 3, ok ? res : rej, 4, false);
		return;
	}
	srv_reply(req, 3, rej, 4, false);
}

/* Deliver everything in flight until nothing moves */
static void pump(void)
{
	poll_udp();
	for (int round = 0; round < 100 && (n_supp || n_srv); round++) {
		static struct msg cur[QN];
		int n = n_srv;
		memcpy(cur, q_srv, sizeof(cur[0]) * n);
		n_srv = 0;
		for (int k = 0; k < n; k++)
			srv_rx(cur[k].d, cur[k].len);
		n = n_supp;
		memcpy(cur, q_supp, sizeof(cur[0]) * n);
		n_supp = 0;
		for (int k = 0; k < n; k++)
			supp_rx(cur[k].port, cur[k].d, cur[k].len);
		poll_udp();
	}
}

static void seconds(int n)
{
	while (n--) {
		dot1x_second();
		pump();
	}
}

static uint8_t st(int port) { return dot1x_ports[port].state; }
static bool open_(int port) { return !(dot1x_blocked >> port & 1); }

static void hex16(const uint8_t *h, char *out) { for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", h[i]); }

static void t_md5(void)
{
	static const struct { const char *in, *out; } v[] = {	/* RFC 1321 A.5 */
		{ "", "d41d8cd98f00b204e9800998ecf8427e" },
		{ "a", "0cc175b9c0f1b6a831c399e269772661" },
		{ "abc", "900150983cd24fb0d6963f7d28e17f72" },
		{ "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
		{ "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
		{ "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
		  "57edf4a22be3c955ac49da2e2107b67a" },
	};
	uint8_t h[16];
	char x[33];
	int ok = 1;
	for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		dot1x_md5((const uint8_t *)v[i].in, strlen(v[i].in), NULL, 0, h);
		hex16(h, x);
		ok &= !strcmp(x, v[i].out);
	}
	check(ok, "MD5: the RFC 1321 test suite");
	/* RFC 2202 test cases 1 and 2 */
	uint8_t key[16];
	memset(key, 0x0b, 16);
	dot1x_hmac_md5(key, 16, (const uint8_t *)"Hi There", 8, h);
	hex16(h, x);
	ok = !strcmp(x, "9294727a3638bb1c13f48ef8158bfc9d");
	dot1x_hmac_md5((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, h);
	hex16(h, x);
	ok &= !strcmp(x, "750c783e6ab0b503eaa86e310a5db738");
	check(ok, "HMAC-MD5: RFC 2202 cases 1 and 2");
}

int main(void)
{
	t_md5();
	dot1x_init();
	dot1x_on = true;
	memcpy(dot1x_cfg.radius_ip, "\xc0\xa8\x0a\x01", 4);
	strcpy(dot1x_cfg.secret, nas_secret);
	dot1x_cfg.mode[4] = DOT1X_AUTO;
	dot1x_cfg.mode[5] = DOT1X_AUTO;
	dot1x_cfg.mode[6] = DOT1X_AUTO;
	dot1x_cfg.mab[6] = true;
	dot1x_cfg.mode[8] = DOT1X_FORCE_UNAUTH;
	dot1x_apply();
	check(rma_act == RTL837X_RMA_ACT_TRAP && gate_mask == (1 << 4 | 1 << 5 | 1 << 6 | 1 << 8), "auto ports wait, force-unauth is blocked, the rest open: %03x", gate_mask);

	/* EAP-MD5 on port 4 (front port 2) */
	supp[4] = (struct supp){ true, "alice", "secret", { 0x02, 0, 0, 0, 0, 0x04 }, 0, 0, 0, 0 };
	link_(4, true);
	seconds(2);
	check(supp[4].id_requests == 1 && supp[4].successes == 1, "port 2: Request/Identity, then EAP Success");
	check(st(4) == DOT1X_S_AUTHENTICATED && open_(4) && !(gate_mask >> 4 & 1), "port 2 authorized and open");
	check(!strcmp(dot1x_ports[4].identity, "alice") && dot1x_ports[4].method == DOT1X_M_EAP, "identity alice, by EAP");
	check(!strcmp(last_calling, "02-00-00-00-00-04") && last_nas_port == 2 && last_service == 2,
	      "Calling-Station-Id %s, NAS-Port %u, Service-Type Framed", last_calling, last_nas_port);
	check(srv_bad_ma == 0, "every Request's Message-Authenticator checks out");

	/* A wrong password: rejected, held, then asked again */
	supp[5] = (struct supp){ true, "bob", "wrong", { 0x02, 0, 0, 0, 0, 0x05 }, 0, 0, 0, 0 };
	link_(5, true);
	seconds(2);
	check(supp[5].failures == 1 && st(5) == DOT1X_S_HELD && !open_(5), "port 3: wrong password, EAP Failure, held and closed");
	supp[5].password = "secret";
	seconds(DOT1X_QUIET_PERIOD - 1);	/* the failure came in the second of the two seconds above */
	check(supp[5].id_requests == 1 && !open_(5), "nothing during the quiet period (%d requests)", supp[5].id_requests);
	seconds(1);
	check(supp[5].id_requests == 2 && st(5) == DOT1X_S_AUTHENTICATED && open_(5),
	      "then it asks again and lets the right password in");

	/* Logoff */
	uint8_t logoff[4] = { 2, 2, 0, 0 };
	dot1x_eapol_rx(5, supp[5].mac, logoff, 4);
	check(!open_(5) && st(5) == DOT1X_S_CONNECTING, "EAPOL-Logoff closes the port");

	/* MAB on port 6: a device without a supplicant */
	link_(6, true);
	uint8_t dev[6] = { 0x02, 0, 0, 0, 0, 0x03 };
	seconds(1);
	check(dot1x_seen(6, dev) == 1, "what a closed port sends is dropped");
	seconds(DOT1X_TX_PERIOD * DOT1X_MAB_AFTER);
	check(srv_mab_ok == 0 && st(6) == DOT1X_S_CONNECTING, "no MAB before %d unanswered requests", DOT1X_MAB_AFTER);
	seconds(2);
	check(srv_mab_ok == 1 && st(6) == DOT1X_S_AUTHENTICATED && open_(6) && dot1x_ports[6].method == DOT1X_M_MAB
	      && last_service == 10, "MAB: the MAC as user and hidden password, Call-Check, accepted");

	/* An EAP-TLS-sized exchange: EAP-Message split and joined at 253 bytes */
	srv_big_mode = true;
	dot1x_restart(4);
	seconds(2);
	check(supp[4].big_seen == 900 && srv_big == 700, "a 900-byte request and a 700-byte response cross whole (%d, %d)",
	      supp[4].big_seen, srv_big);
	check(open_(4), "and the port opens");
	srv_big_mode = false;

	/* Re-authentication: the port stays open while it runs */
	dot1x_cfg.reauth_s = 100;
	dot1x_restart(4);
	seconds(2);
	int ids = supp[4].id_requests;
	seconds(99);
	check(supp[4].id_requests == ids + 1 && open_(4), "re-authentication after 100 s, port open throughout");
	supp[4].password = "wrong";
	seconds(100);
	check(!open_(4) && st(4) == DOT1X_S_HELD, "a failed re-authentication closes it");
	supp[4].password = "secret";
	dot1x_cfg.reauth_s = 0;

	/* Session-Timeout from the server sets the re-authentication */
	session_timeout = 600;
	seconds(DOT1X_QUIET_PERIOD + 1);
	check(open_(4) && dot1x_ports[4].reauth_left > 590, "Session-Timeout 600: re-authentication due in %u s",
	      dot1x_ports[4].reauth_left);
	session_timeout = 0;

	/* The server down: three tries, then held */
	server_down = true;
	dot1x_restart(4);
	int s0 = radius_sent;
	seconds(2 + DOT1X_SERVER_TIMEOUT * DOT1X_SERVER_TRIES);
	check(radius_sent - s0 == DOT1X_SERVER_TRIES && st(4) == DOT1X_S_HELD && !open_(4),
	      "no server: %d tries, then held", radius_sent - s0);
	server_down = false;

	/* A server with another secret: its replies don't verify, so nothing opens */
	srv_secret = "other";
	dot1x_restart(5);
	seconds(2 + DOT1X_SERVER_TIMEOUT * DOT1X_SERVER_TRIES);
	check(!open_(5) && st(5) == DOT1X_S_HELD, "replies signed with another secret are ignored");
	srv_secret = "testing123";

	/* Link down */
	link_(6, false);
	check(!open_(6) && st(6) == DOT1X_S_DOWN, "link down: closed, waiting for the link");

	/* A port set to auto with its link already up: no link change will come */
	supp[7] = (struct supp){ true, "alice", "secret", { 0x02, 0, 0, 0, 0, 0x07 }, 0, 0, 0, 0 };
	links_up = 1 << 7;
	dot1x_cfg.mode[7] = DOT1X_AUTO;
	dot1x_apply();
	seconds(2);
	check(supp[7].id_requests == 1 && st(7) == DOT1X_S_AUTHENTICATED && open_(7),
	      "auto on a port already up: asked at once, authorized");

	/* Off: everything open, EAPOL no longer trapped */
	dot1x_on = false;
	dot1x_apply();
	check(rma_act == RTL837X_RMA_ACT_FORWARD && gate_mask == 0 && st(4) == DOT1X_S_OFF, "802.1X off: no port gated");

	dot1x_on = true;
	dot1x_apply();
	links_up = 0x1f8;
	dot1x_cfg.mode[3] = dot1x_cfg.mode[8] = DOT1X_AUTO;
	dot1x_apply();
	for (int k = 3; k < 9; k++)
		supp[k] = (struct supp){ true, "alice", "secret", { 2, 0, 0, 0, 0, k }, 0, 0, 0, 0 };
	seconds(12);
	int up = 0;
	for (int k = 3; k < 9; k++)
		up += st(k) == DOT1X_S_AUTHENTICATED;
	check(up == 6, "six ports at once over four request buffers: %d authenticated", up);
	check(dot1x_seen(4, supp[4].mac) == 0, "an open port's other frames pass");
	uint8_t big[1500] = { 2, 0, 1404 >> 8, 1404 & 0xff, 2, 0, 1404 >> 8, 1404 & 0xff, 13 };
	dot1x_restart(4);
	supp[4].present = false;
	seconds(1);
	uint8_t id[] = { 2, 0, 0, 10, 2, dot1x_ports[4].eap_id, 0, 10, 1, 'a', 'l', 'i', 'c', 'e' };
	dot1x_eapol_rx(4, supp[4].mac, id, sizeof(id));
	pump();
	big[5] = dot1x_ports[4].last_id;
	dot1x_eapol_rx(4, supp[4].mac, big, 1408);
	check(st(4) == DOT1X_S_HELD, "an EAP packet that does not fit a datagram fails cleanly");
	links_up &= ~(1 << 5);
	links_up |= 1 << 5;
	dot1x_link_change(1 << 5);
	check(!open_(5), "a link that bounced is closed even if it is back");
	check(!bad_frames, "every EAPOL frame is well formed, outside the management VLAN (%d bad)", bad_frames);
	check(management_vlan == 5, "management VLAN restored");

	printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
