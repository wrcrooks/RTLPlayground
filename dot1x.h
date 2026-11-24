#ifndef _DOT1X_H_
#define _DOT1X_H_

/*
 * 802.1X port authentication: the switch as authenticator, a RADIUS server as
 * authentication server (EAP relayed per RFC 3579), and MAC authentication
 * (MAB) for devices without a supplicant. See doc/dot1x.md.
 */

#include <stdint.h>
#include <stdbool.h>

#define DOT1X_PORTS		9
#define DOT1X_SECRET_MAX	64
#define DOT1X_IDENTITY_MAX	64
#define DOT1X_STATE_MAX		128

/* Port control */
#define DOT1X_FORCE_AUTH	0	/* no authentication: the default */
#define DOT1X_AUTO		1	/* authenticate whatever is connected */
#define DOT1X_FORCE_UNAUTH	2	/* blocked */

/* Port states */
#define DOT1X_S_OFF		0	/* force-auth, or 802.1X off */
#define DOT1X_S_DOWN		1	/* auto, link down */
#define DOT1X_S_CONNECTING	2	/* asking for an identity */
#define DOT1X_S_AUTHENTICATING	3	/* talking to the RADIUS server */
#define DOT1X_S_AUTHENTICATED	4
#define DOT1X_S_HELD		5	/* after a failure, the quiet period */
#define DOT1X_S_BLOCKED		6	/* force-unauth */

#define DOT1X_M_NONE		0
#define DOT1X_M_EAP		1
#define DOT1X_M_MAB		2

/* Timing, seconds (802.1X-2004 defaults where it has one) */
#define DOT1X_TX_PERIOD		30	/* between EAP Request/Identity */
#define DOT1X_MAB_AFTER		3	/* requests without an answer before MAB */
#define DOT1X_SUPP_TIMEOUT	30
#define DOT1X_QUIET_PERIOD	60
#define DOT1X_SERVER_FIRST	2	/* the first try may only have resolved the server's MAC */
#define DOT1X_SERVER_TIMEOUT	5
#define DOT1X_SERVER_TRIES	4

struct dot1x_config {
	uint8_t radius_ip[4];
	uint16_t radius_port;
	char secret[DOT1X_SECRET_MAX + 1];
	uint8_t mode[DOT1X_PORTS];
	uint8_t mab[DOT1X_PORTS];
	uint32_t reauth_s;		/* 0: no periodic re-authentication */
};

struct dot1x_port {
	uint8_t state;
	uint8_t method;			/* DOT1X_M_* of the last success */
	uint8_t run;			/* DOT1X_M_* of the run in progress */
	uint8_t authorized;
	uint8_t mac_valid;
	uint8_t mac[6];			/* the supplicant's, or the MAB device's */
	char identity[DOT1X_IDENTITY_MAX];
	uint16_t ok, fail;
	uint32_t reauth_left;
	uint8_t timer;			/* seconds until the next step */
	uint8_t tx_count;		/* Request/Identity sent without an answer */
	uint8_t eap_id;			/* id of our last Request/Identity */
	uint8_t last_id;		/* id of the last EAP request sent on */
	uint8_t slot;			/* the outstanding RADIUS request, or DOT1X_NO_SLOT */
	uint8_t state_len;
	uint8_t state_attr[DOT1X_STATE_MAX];
};

#define DOT1X_NO_SLOT		0xff

extern __xdata uint8_t dot1x_on;
extern __xdata struct dot1x_config dot1x_cfg;
extern __xdata struct dot1x_port dot1x_ports[DOT1X_PORTS];
extern __xdata uint16_t dot1x_blocked;	/* ports that may only exchange EAPOL */

void dot1x_init(void) __banked;
void dot1x_apply(void) __banked;		/* after a configuration change */
uint8_t dot1x_rx(void) __banked;		/* frame in uip_buf: 1 if taken or dropped */
void dot1x_second(void) __banked;
void dot1x_link_change(uint16_t changed) __banked;
void dot1x_callback(uint16_t lport) __banked;
void dot1x_restart(uint8_t port) __banked;
void dot1x_cmd(void) __banked;
void send_dot1x(void) __banked;

#endif
