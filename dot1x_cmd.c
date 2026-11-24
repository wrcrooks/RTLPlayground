/*
 * The dot1x command and /dot1x.json:
 *   dot1x                                    status
 *   dot1x on|off
 *   dot1x radius <a.b.c.d> [<udp port>]      the RADIUS server, port 1812 by default
 *   dot1x secret <text>                      its shared secret
 *   dot1x reauth <60-86400>|off              periodic re-authentication
 *   dot1x port <n> auto|force-auth|force-unauth
 *   dot1x port <n> mab on|off                MAC authentication for devices without a supplicant
 *   dot1x port <n> restart                   authenticate again now
 */

#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "machine.h"
#include "cmd_parser.h"
#include "page_impl.h"
#include "dot1x.h"

#pragma codeseg BANK4
#pragma constseg BANK4

extern __code const struct machine machine;
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[15];
extern __xdata uint8_t outbuf[TCP_OUTBUF_SIZE];
extern __xdata uint16_t slen;

static __xdata uint8_t c_w, c_i, c_p, c_port;
static __xdata uint32_t c_num, c_max, c_t;
static __xdata uint8_t c_html;
static __xdata uint8_t c_ip[4];
static __xdata struct dot1x_port * __xdata c_s;

static __code const uint32_t c_tens[] = { 1000000000, 100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1 };


static void c_char(char c)
{
	if (c_html)
		char_to_html(c);
	else
		write_char(c);
}


static void c_str(__code const char *s)
{
	while (*s)
		c_char(*s++);
}


/* c_num in decimal */
static void c_dec(void)
{
	static __xdata uint8_t k, d, any;

	any = 0;
	for (k = 0; k < 10; k++) {
		c_t = c_tens[k];
		d = '0';
		while (c_num >= c_t) {
			c_num -= c_t;
			d++;
		}
		if (d != '0' || any || k == 9) {
			any = 1;
			c_char(d);
		}
	}
}


static void c_addr(void)
{
	for (c_i = 0; c_i < 4; c_i++) {
		if (c_i)
			c_char('.');
		c_num = dot1x_cfg.radius_ip[c_i];
		c_dec();
	}
}


static void c_state(void)
{
	switch (c_s->state) {
	case DOT1X_S_OFF: c_str("off"); break;
	case DOT1X_S_DOWN: c_str("down"); break;
	case DOT1X_S_CONNECTING: c_str("connecting"); break;
	case DOT1X_S_AUTHENTICATING: c_str("authenticating"); break;
	case DOT1X_S_AUTHENTICATED: c_str("authenticated"); break;
	case DOT1X_S_HELD: c_str("held"); break;
	default: c_str("blocked");
	}
}


static void c_mode(void)
{
	if (dot1x_cfg.mode[c_port] == DOT1X_AUTO)
		c_str("auto");
	else if (dot1x_cfg.mode[c_port] == DOT1X_FORCE_UNAUTH)
		c_str("force-unauth");
	else
		c_str("force-auth");
}


/* The identity came from the port: printable characters only, and it may not break out of JSON */
static void c_identity(void)
{
	static __xdata char ch;

	for (c_i = 0; c_i < 40 && c_s->identity[c_i]; c_i++) {
		ch = c_s->identity[c_i];
		if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\' || ch == '<' || ch == '&')
			ch = '.';
		c_char(ch);
	}
}


static void c_status(void)
{
	c_html = 0;
	c_str("802.1X ");
	c_str(dot1x_on ? "on" : "off");
	c_str(", RADIUS ");
	c_addr();
	c_char(':');
	c_num = dot1x_cfg.radius_port;
	c_dec();
	c_str(dot1x_cfg.secret[0] ? ", secret set" : ", secret not set");
	if (dot1x_cfg.reauth_s) {
		c_str(", re-authentication every ");
		c_num = dot1x_cfg.reauth_s;
		c_dec();
		c_str(" s");
	}
	c_char('\n');
	for (c_port = MACHINE_MIN_PORT; c_port <= MACHINE_MAX_PORT; c_port++) {
		if (dot1x_cfg.mode[c_port] == DOT1X_FORCE_AUTH)
			continue;
		c_s = &dot1x_ports[c_port];
		c_str("  port ");
		c_char('0' + machine.log_to_phys_port[c_port]);
		c_str(": ");
		c_mode();
		if (dot1x_cfg.mab[c_port])
			c_str(" + MAB");
		c_str(", ");
		c_state();
		c_str(!dot1x_on || c_s->authorized ? ", open" : ", closed");
		if (c_s->identity[0]) {
			c_str(c_s->run == DOT1X_M_MAB ? ", MAC " : ", user ");
			c_identity();
		}
		c_char('\n');
	}
}


/* Word c_w is s */
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


/* Decimal number at c_p up to c_max, into c_num */
static uint8_t c_number(void)
{
	static __xdata uint8_t d, n;

	c_num = 0;
	for (n = 0; n < 6; n++) {
		d = cmd_buffer[c_p] - '0';
		if (d > 9)
			break;
		c_num <<= 1;
		c_t = c_num;
		c_num <<= 2;
		c_num += c_t;
		c_num += d;
		c_p++;
	}
	return n && c_num <= c_max;
}


static uint8_t c_end(void)
{
	return cmd_buffer[c_p] == ' ' || !cmd_buffer[c_p];
}


static uint8_t c_radius(void)
{
	if (cmd_words_len != 3 && cmd_words_len != 4)
		return 0;
	c_p = cmd_words_b[2];
	c_max = 255;
	for (c_i = 0; c_i < 4; c_i++) {
		if (c_i && cmd_buffer[c_p++] != '.')
			return 0;
		if (!c_number())
			return 0;
		c_ip[c_i] = c_num;
	}
	if (!c_end())
		return 0;
	c_num = 1812;
	if (cmd_words_len == 4) {
		c_p = cmd_words_b[3];
		c_max = 65535;
		if (!c_number() || !c_end() || !c_num)
			return 0;
	}
	for (c_i = 0; c_i < 4; c_i++)
		dot1x_cfg.radius_ip[c_i] = c_ip[c_i];
	dot1x_cfg.radius_port = c_num;
	return 1;
}


static uint8_t c_secret(void)
{
	if (cmd_words_len < 3)
		return 0;
	c_p = cmd_words_b[2];
	for (c_i = 0; cmd_buffer[c_p + c_i]; c_i++)
		if (c_i == DOT1X_SECRET_MAX)
			return 0;
	for (c_i = 0; cmd_buffer[c_p + c_i]; c_i++)
		dot1x_cfg.secret[c_i] = cmd_buffer[c_p + c_i];
	dot1x_cfg.secret[c_i] = 0;
	return 1;
}


static uint8_t c_reauth(void)
{
	if (cmd_words_len != 3)
		return 0;
	c_w = 2;
	if (c_eq("off")) {
		dot1x_cfg.reauth_s = 0;
		return 1;
	}
	c_p = cmd_words_b[2];
	c_max = 86400;
	if (!c_number() || !c_end() || c_num < 60)
		return 0;
	dot1x_cfg.reauth_s = c_num;
	return 1;
}


static uint8_t c_port_cmd(void)
{
	if (cmd_words_len < 4)
		return 0;
	c_p = cmd_words_b[2];
	c_port = cmd_buffer[c_p++] - '1';
	if (c_port > 8 || !c_end())
		return 0;
	c_port = machine.phys_to_log_port[c_port];
	if (!MACHINE_PORT_VALID(c_port))
		return 0;
	c_w = 3;
	if (cmd_words_len == 5) {
		if (!c_eq("mab"))
			return 0;
		c_w = 4;
		if (c_eq("on"))
			dot1x_cfg.mab[c_port] = 1;
		else if (c_eq("off"))
			dot1x_cfg.mab[c_port] = 0;
		else
			return 0;
		return 1;
	}
	if (cmd_words_len != 4)
		return 0;
	if (c_eq("auto"))
		dot1x_cfg.mode[c_port] = DOT1X_AUTO;
	else if (c_eq("force-auth"))
		dot1x_cfg.mode[c_port] = DOT1X_FORCE_AUTH;
	else if (c_eq("force-unauth"))
		dot1x_cfg.mode[c_port] = DOT1X_FORCE_UNAUTH;
	else if (c_eq("restart"))
		dot1x_restart(c_port);
	else
		return 0;
	return 1;
}


void dot1x_cmd(void) __banked
{
	static __xdata uint8_t ok;

	if (cmd_words_len == 1) {
		c_status();
		return;
	}
	ok = 0;
	c_w = 1;
	if (c_eq("on") && cmd_words_len == 2)
		ok = dot1x_on = 1;
	else if (c_eq("off") && cmd_words_len == 2) {
		dot1x_on = 0;
		ok = 1;
	} else if (c_eq("radius"))
		ok = c_radius();
	else if (c_eq("secret"))
		ok = c_secret();
	else if (c_eq("reauth"))
		ok = c_reauth();
	else if (c_eq("port"))
		ok = c_port_cmd();
	if (ok) {
		dot1x_apply();
		return;
	}
	err_status = ERR_INVALID_ARGUMENT;
	print_string("dot1x [on|off|radius <a.b.c.d> [<udp port>]|secret <text>|reauth <60-86400>|off]\n"
		     "dot1x port <n> auto|force-auth|force-unauth|restart|mab on|off\n");
}


static void j_key(__code const char *s)
{
	c_str(",\"");
	c_str(s);
	c_str("\":");
}


void send_dot1x(void) __banked
{
	c_html = 1;
	slen = strtox(outbuf, HTTP_RESPONSE_JSON);
	c_str("{\"enabled\":");
	bool_to_html(dot1x_on);
	j_key("radius");
	c_char('"');
	c_addr();
	c_char('"');
	j_key("radius_port");
	c_num = dot1x_cfg.radius_port;
	c_dec();
	j_key("secret");
	bool_to_html(dot1x_cfg.secret[0]);
	j_key("reauth");
	c_num = dot1x_cfg.reauth_s;
	c_dec();
	j_key("ports");
	c_char('[');
	for (c_port = MACHINE_MIN_PORT; c_port <= MACHINE_MAX_PORT; c_port++) {
		c_s = &dot1x_ports[c_port];
		c_str(c_port == MACHINE_MIN_PORT ? "{\"port\":" : ",{\"port\":");
		itoa_html(machine.log_to_phys_port[c_port]);
		j_key("mode");
		c_char('"');
		c_mode();
		c_char('"');
		j_key("mab");
		bool_to_html(dot1x_cfg.mab[c_port]);
		j_key("state");
		c_char('"');
		c_state();
		c_char('"');
		j_key("authorized");
		bool_to_html(c_s->authorized);
		j_key("method");
		c_str(c_s->method == DOT1X_M_MAB ? "\"mab\"" : c_s->method == DOT1X_M_EAP ? "\"eap\"" : "\"\"");
		j_key("mac");
		c_char('"');
		for (c_i = 0; c_s->mac_valid && c_i < 6; c_i++) {
			if (c_i)
				c_char(':');
			byte_to_html(c_s->mac[c_i]);
		}
		c_char('"');
		j_key("user");
		c_char('"');
		c_identity();
		c_char('"');
		j_key("ok");
		c_num = c_s->ok;
		c_dec();
		j_key("fail");
		c_num = c_s->fail;
		c_dec();
		j_key("reauth_left");
		c_num = c_s->reauth_left;
		c_dec();
		c_char('}');
	}
	c_str("]}");
}
