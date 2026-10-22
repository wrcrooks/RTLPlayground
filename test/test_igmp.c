/*
 * Host-side test for IGMP snooping.
 *
 * Drives real IGMP reports and leaves through the UNMODIFIED
 * igmp_packet_handler() and checks the resulting membership in the mocked
 * ASIC L2/L3MC table (hw_mock.c). The handler finds, stores and clears group
 * entries with a by-key table search (TBL_LUTREAD_MAC), which the mock
 * models, so a join leaves the group's entry with the ingress port set and a
 * final leave removes it.
 *
 * Run: make -C test    (exit code 0 = all checks pass)
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_igmp.h"
#include "rtl837x_port.h"
#include "uip.h"
#include "uip_arp.h"
#include "hw_mock.h"

/* A tiny harness of its own: support.c pulls in the command-editor mocks this
 * test has no use for. */
static int tests_run, tests_failed;
#define CHECK(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { tests_failed++; printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); } \
	else printf("  ok:   %s\n", (msg)); \
} while (0)

/* Firmware console leaves not already in env_tables; discard output here */
void print_sfr_data(void) { }
void print_string(const char *s) { (void)s; }
void write_char(char c) { (void)c; }
void itoa(uint8_t v) { (void)v; }

/* Mirrors struct igmp_pkt in rtl837x_igmp.c: an IPv4 packet with a 24-byte
 * header (the router-alert option, so IHL is 6) carrying IGMP. Kept here so a
 * test can address the fields; the production struct stays private. */
struct igmp_pkt_t {
	uint8_t ipv4mc_addr[6];
	uint8_t src_addr[6];
	struct rtl_tag rtl_tag;
	uint16_t ipv4_tag;
	uint8_t hlen;
	uint8_t dscp;
	uint16_t len;
	uint16_t id;
	uint16_t flags;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;
	uint8_t src_ip[4];
	uint8_t dst_ip[4];
	uint8_t ip_opt;
	uint8_t ip_len;
	uint16_t ra;
	uint8_t igmp_type;
	uint8_t igmp_res1;
	uint16_t igmp_checksum;
	uint16_t igmp_res2;
	uint16_t igmp_records;
	uint8_t igmp_rtype;
	uint8_t igmp_auxlen;
	uint16_t igmp_nsrc;
	uint8_t mc_ip[4];
};

/* Provided here: the handler's environment that no mock already carries */
uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
uint8_t igmpEnabled;
uint8_t cpuPort = 0;

#define PKT		((struct igmp_pkt_t *)uip_buf)
#define REC0_OFS	offsetof(struct igmp_pkt_t, igmp_rtype)
#define INGRESS_PORT	3

/* Port the next packet arrives on, as the CPU tag reports it */
static uint8_t pkt_port = INGRESS_PORT;

/* ---- packet construction ------------------------------------------------- */

static uint16_t pkt_len;

static void pkt_begin(uint8_t igmp_type)
{
	memset(uip_buf, 0, sizeof(uip_buf));
	PKT->ipv4_tag = HTONS(UIP_ETHTYPE_IP);
	PKT->hlen = 0x46;		/* IPv4, IHL 6 (20 + router alert) */
	PKT->protocol = IGMP_PROTO;
	PKT->rtl_tag.pmask = pkt_port << 8;	/* handler reads (pmask >> 8) & 0xf */
	PKT->igmp_type = igmp_type;
	pkt_len = REC0_OFS;		/* v1/v2 overwrite the group below */
}

/* v1/v2/leave: one group right after the 8-byte IGMP header */
static void pkt_group(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	uint8_t *g = &uip_buf[offsetof(struct igmp_pkt_t, igmp_res2)];
	g[0] = a; g[1] = b; g[2] = c; g[3] = d;
	pkt_len = offsetof(struct igmp_pkt_t, igmp_res2) + 4;
}

/* v3: append a group record (no sources, no aux data) */
static uint16_t nrec;
static void pkt_v3_begin(void) { pkt_begin(IGMP_V3_REPORT); nrec = 0; pkt_len = REC0_OFS; }
static void pkt_v3_record(uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	uint8_t *r = &uip_buf[pkt_len];
	r[0] = type; r[1] = 0; r[2] = 0; r[3] = 0;	/* type, auxlen, nsrc */
	r[4] = a; r[5] = b; r[6] = c; r[7] = d;		/* group address */
	pkt_len += 8;
	nrec++;
}
static void pkt_v3_finish(void)
{
	PKT->igmp_records = HTONS(nrec);
}

static uint8_t sent[UIP_CONF_BUFFER_ALLOC_SIZE];	/* the frame as delivered */

static void deliver(void)
{
	PKT->len = HTONS(pkt_len - offsetof(struct igmp_pkt_t, hlen));	/* IP total length */
	memcpy(sent, uip_buf, pkt_len);
	uip_len = pkt_len;
	igmp_packet_handler();
}

/* ---- membership readout from the mock table ------------------------------ */

/* The handler keys a group by sip=0 and the dip, which the mock decodes to
 * mac = {dip[2], dip[3], 0,0,0,0}, vid = ((dip[0] & 0xf) << 8) | dip[1]. */
static const struct hw_l2 *group_entry(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	uint8_t mac[6] = { c, d, 0, 0, 0, 0 };
	return hw_l2_find(mac, ((a & 0xf) << 8) | b);
}

static int group_pmask(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	const struct hw_l2 *e = group_entry(a, b, c, d);
	uint8_t mac[6]; uint16_t vid, pm; bool v, s, mc;
	if (!e)
		return -1;
	hw_l2_decode(e, mac, &vid, &pm, &v, &s, &mc);
	return pm;
}

/* Seed a group that already has the ingress port joined, as a prior report
 * would have left it. idx is the table slot, so a test can force idx 0. */
static void seed_group(uint16_t idx, uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	uint8_t mac[6] = { c, d, 0, 0, 0, 0 };
	hw_l2_put(idx, mac, ((a & 0xf) << 8) | b, 1u << INGRESS_PORT, false);
}

/* ---- scenarios ----------------------------------------------------------- */

static void scenario_v2_join_leave(void)
{
	hw_reset();
	igmpEnabled = 1;
	pkt_begin(IGMP_V2_REPORT);
	pkt_group(239, 1, 1, 1);
	deliver();
	CHECK(group_pmask(239, 1, 1, 1) == (1 << INGRESS_PORT),
	      "v2 report joins the group with the ingress port");

	pkt_begin(IGMP_V2_LEAVE);
	pkt_group(239, 1, 1, 1);
	deliver();
	CHECK(group_entry(239, 1, 1, 1) == 0,
	      "v2 leave removes the group once its last port is gone");
}

static void scenario_v3_exclude_is_join(void)
{
	/* The common periodic report: a host lists each joined group as
	 * MODE_IS_EXCLUDE (type 2). The old code only acted on types 3 and 4,
	 * so these groups were never snooped and dropped after a reboot. */
	hw_reset();
	igmpEnabled = 1;
	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_MODE_IS_EXCLUDE, 239, 1, 1, 1);
	pkt_v3_finish();
	deliver();
	CHECK(group_pmask(239, 1, 1, 1) == (1 << INGRESS_PORT),
	      "v3 MODE_IS_EXCLUDE joins the group");
}

static void scenario_v3_all_records(void)
{
	/* A single report carrying several group records: every one is
	 * snooped, not just the first. */
	hw_reset();
	igmpEnabled = 1;
	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_TO_EXCLUDE, 239, 1, 1, 1);
	pkt_v3_record(IGMP_V3_MODE_IS_EXCLUDE, 239, 2, 2, 2);
	pkt_v3_record(IGMP_V3_ALLOW_NEW, 239, 3, 3, 3);
	pkt_v3_finish();
	deliver();
	CHECK(group_pmask(239, 1, 1, 1) == (1 << INGRESS_PORT)
	      && group_pmask(239, 2, 2, 2) == (1 << INGRESS_PORT)
	      && group_pmask(239, 3, 3, 3) == (1 << INGRESS_PORT),
	      "v3 report with several records snoops every group");
}

static void scenario_v3_include_is_leave(void)
{
	hw_reset();
	igmpEnabled = 1;
	seed_group(5, 239, 4, 4, 4);
	CHECK(group_pmask(239, 4, 4, 4) == (1 << INGRESS_PORT), "seed present");

	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_TO_INCLUDE, 239, 4, 4, 4);	/* no sources = leave */
	pkt_v3_finish();
	deliver();
	CHECK(group_entry(239, 4, 4, 4) == 0,
	      "v3 CHANGE_TO_INCLUDE with no sources leaves the group");
}

static void scenario_delete_at_index_zero(void)
{
	/* A group whose table slot is index 0: the old leave path guarded
	 * deletion with `idx != 0`, so an emptied group at slot 0 was never
	 * removed and lingered forever. */
	hw_reset();
	igmpEnabled = 1;
	seed_group(0, 239, 5, 5, 5);
	CHECK(group_pmask(239, 5, 5, 5) == (1 << INGRESS_PORT), "seed at idx 0 present");

	pkt_begin(IGMP_V2_LEAVE);
	pkt_group(239, 5, 5, 5);
	deliver();
	CHECK(group_entry(239, 5, 5, 5) == 0,
	      "a group emptied at table index 0 is deleted");
}

static void scenario_block_and_unknown_ignored(void)
{
	hw_reset();
	igmpEnabled = 1;
	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_BLOCK_OLD, 239, 6, 6, 6);	/* needs per-source state */
	pkt_v3_record(0x00, 239, 7, 7, 7);		/* unknown record type */
	pkt_v3_finish();
	deliver();
	CHECK(group_entry(239, 6, 6, 6) == 0 && group_entry(239, 7, 7, 7) == 0,
	      "v3 BLOCK_OLD_SOURCES and unknown record types change nothing");
}

static void scenario_truncated_report(void)
{
	/* The record count claims three groups but the packet ends after one:
	 * the walk must stop at the packet, not read past it. */
	hw_reset();
	igmpEnabled = 1;
	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_MODE_IS_EXCLUDE, 239, 8, 8, 8);
	nrec = 3;
	pkt_v3_finish();
	deliver();
	CHECK(group_pmask(239, 8, 8, 8) == (1 << INGRESS_PORT),
	      "the group that is present is snooped");
	CHECK(hw_l2_count() == 1,
	      "a record count past the packet end adds no phantom groups");
}


/* ---- relay: what the switch passes on ------------------------------------ */

/* Ports the relayed frame's CPU tag sends it to, 0 when nothing is relayed */
static uint16_t relayed(void)
{
	struct rtl_tag *t = (struct rtl_tag *)&uip_buf[RTL_FRAME_DESC_SIZE + 12];
	return uip_len ? NTOHS(t->pmask) : 0;
}

/* The relayed frame is the delivered one behind the TX descriptor, with a TX
 * CPU tag, cut at the IP length and padded to the Ethernet minimum */
static int relayed_frame_ok(void)
{
	struct rtl_tag *t = (struct rtl_tag *)&uip_buf[RTL_FRAME_DESC_SIZE + 12];
	uint8_t *f = &uip_buf[RTL_FRAME_DESC_SIZE];
	uint16_t want = pkt_len < 68 ? 68 : pkt_len;

	if (uip_len != want || NTOHS(t->tag) != RTL_FRAME_TAG_ID
	    || t->version != RTL_FRAME_TAG_VERSION || NTOHS(t->flags) != RTL_TAG_LEARN_DIS)
		return 0;
	if (memcmp(f, sent, 12) || memcmp(f + 20, sent + 20, pkt_len - 20))
		return 0;
	for (uint16_t i = pkt_len; i < want; i++)
		if (f[i])
			return 0;
	return 1;
}

#define ALL_PORTS	0x1ff	/* the test machine's logical ports 0-8 */
#define BIT(p)		(1u << (p))
#define ROUTER		1	/* port the multicast router queries from */

static void make_vlan(uint16_t vid, uint16_t members)
{
	vlan_settings.vlan = vid;
	vlan_settings.members = members;
	vlan_settings.tagged = 0;
	vlan_create();
}

/* 2-bit STP state of port p, as RTL837X_MSTP_STATES holds it */
static void stp_state(uint8_t p, uint8_t state)
{
	uint32_t v = hw_reg_get(RTL837X_MSTP_STATES);
	hw_reg_set(RTL837X_MSTP_STATES, (v & ~(3u << (2 * p))) | ((uint32_t)state << (2 * p)));
}

/* A switch as it boots: every port forwarding, none isolated, all untagged
 * members of VLAN 1, snooping on and no router port known */
static void relay_env(void)
{
	hw_reset();
	igmp_init();
	igmpEnabled = 1;
	hw_reg_set(RTL837X_MSTP_STATES, 0x0003ffff);
	for (uint8_t p = 0; p <= 8; p++) {
		hw_reg_set(RTL837X_PORT_ISOLATION_BASE + (p << 2), 0x3ff);
		port_pvid_set(p, 1);
	}
	make_vlan(1, ALL_PORTS);
}

static void query_from(uint8_t port)
{
	pkt_port = port;
	pkt_begin(IGMP_QUERY);
	pkt_group(0, 0, 0, 0);
	deliver();
	pkt_port = INGRESS_PORT;
}

static void report_from(uint8_t port, uint8_t type)
{
	pkt_port = port;
	pkt_begin(type);
	pkt_group(239, 9, 9, 9);
	deliver();
	pkt_port = INGRESS_PORT;
}

static void scenario_relay_query_floods(void)
{
	relay_env();
	query_from(ROUTER);
	CHECK(relayed() == (ALL_PORTS & ~BIT(ROUTER)),
	      "a query goes to every other port");
	CHECK(relayed_frame_ok(),
	      "relayed frame: TX tag, payload intact, cut at IP length, padded to 68");
}

static void scenario_relay_reports_to_router(void)
{
	relay_env();
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == 0, "with no router port a report is not passed on");
	CHECK(group_entry(239, 9, 9, 9) != 0, "...but the switch still snoops it");

	query_from(ROUTER);
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == BIT(ROUTER), "after a query, a v2 report goes only to the router port");
	CHECK(relayed_frame_ok(), "relayed report frame is intact");

	pkt_begin(IGMP_V3_REPORT);
	pkt_v3_begin();
	pkt_v3_record(IGMP_V3_MODE_IS_EXCLUDE, 239, 9, 9, 10);
	pkt_v3_finish();
	deliver();
	CHECK(relayed() == BIT(ROUTER), "a v3 report goes only to the router port");

	report_from(INGRESS_PORT, IGMP_V2_LEAVE);
	CHECK(relayed() == BIT(ROUTER), "a leave goes only to the router port");

	report_from(ROUTER, IGMP_V2_REPORT);
	CHECK(relayed() == 0, "a report from the router port is not sent back to it");
}

static void scenario_relay_static_router(void)
{
	relay_env();
	igmp_rport_static = BIT(6);
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == BIT(6), "a static router port gets reports without any query");
	query_from(ROUTER);
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == (BIT(6) | BIT(ROUTER)), "static and learned router ports both get reports");
}

static void scenario_relay_router_ages(void)
{
	relay_env();
	query_from(ROUTER);
	for (int s = 0; s < IGMP_RPORT_AGE - 1; s++)
		igmp_tick();
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == BIT(ROUTER), "a router port lasts 254 s after its last query");
	igmp_tick();
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == 0, "...and is gone at 255 s");

	query_from(ROUTER);
	igmp_setup();				/* igmp off */
	igmpEnabled = 1;
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == 0, "turning snooping off forgets learned router ports");
}

static void scenario_relay_respects_forwarding(void)
{
	relay_env();
	stp_state(5, 1);			/* blocking */
	query_from(ROUTER);
	CHECK(!(relayed() & BIT(5)) && (relayed() & BIT(4)),
	      "nothing goes to a port STP is blocking");

	relay_env();
	make_vlan(1, ALL_PORTS & ~BIT(7));
	query_from(ROUTER);
	CHECK(!(relayed() & BIT(7)) && (relayed() & BIT(6)),
	      "nothing goes to a port outside the ingress port's VLAN");

	relay_env();
	hw_reg_set(RTL837X_PORT_ISOLATION_BASE + (ROUTER << 2), 0x3ff & ~BIT(8));
	query_from(ROUTER);
	CHECK(!(relayed() & BIT(8)) && (relayed() & BIT(6)),
	      "nothing goes to a port the ingress port is isolated from");

	relay_env();
	port_lag_members_set(0, BIT(ROUTER) | BIT(2));
	port_lag_members_set(1, BIT(6) | BIT(8));
	query_from(ROUTER);
	CHECK(relayed() == (ALL_PORTS & ~BIT(ROUTER) & ~BIT(2)),
	      "nothing goes back into the ingress LAG; other LAGs get every member");
}

static void scenario_relay_other_types(void)
{
	relay_env();
	pkt_port = ROUTER;
	pkt_begin(0x1e);			/* mtrace response: unknown here */
	pkt_group(0, 0, 0, 0);
	deliver();
	pkt_port = INGRESS_PORT;
	CHECK(relayed() == (ALL_PORTS & ~BIT(ROUTER)), "an unknown IGMP type is flooded");
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == 0, "...without making its port a router port");

	pkt_port = 6;
	pkt_begin(IGMP_PIM_V1);
	pkt_group(0, 0, 0, 0);
	deliver();
	pkt_port = INGRESS_PORT;
	report_from(INGRESS_PORT, IGMP_V2_REPORT);
	CHECK(relayed() == BIT(6), "PIMv1 marks its port as a router port");
}

static void scenario_relay_bounded_by_frame(void)
{
	/* An IP length that claims more than arrived copies only what arrived */
	relay_env();
	pkt_port = ROUTER;
	pkt_begin(IGMP_QUERY);
	pkt_group(0, 0, 0, 0);
	PKT->len = HTONS(1400);
	memcpy(sent, uip_buf, pkt_len);
	uip_len = pkt_len;
	igmp_packet_handler();
	pkt_port = INGRESS_PORT;
	CHECK(uip_len == 68, "an overlong IP length relays only the received frame");
}

int main(void)
{
	printf("== igmp: snooping joins, leaves and record walk ==\n");
	scenario_v2_join_leave();
	scenario_v3_exclude_is_join();
	scenario_v3_all_records();
	scenario_v3_include_is_leave();
	scenario_delete_at_index_zero();
	scenario_block_and_unknown_ignored();
	scenario_truncated_report();
	scenario_relay_query_floods();
	scenario_relay_reports_to_router();
	scenario_relay_static_router();
	scenario_relay_router_ages();
	scenario_relay_respects_forwarding();
	scenario_relay_other_types();
	scenario_relay_bounded_by_frame();

	printf("\n%s (%d checks, %d failed)\n",
	       tests_failed ? "IGMP: FAILURES" : "IGMP: ALL PASS",
	       tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
