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

/* ---- packet construction ------------------------------------------------- */

static uint16_t pkt_len;

static void pkt_begin(uint8_t igmp_type)
{
	memset(uip_buf, 0, sizeof(uip_buf));
	PKT->ipv4_tag = HTONS(UIP_ETHTYPE_IP);
	PKT->hlen = 0x46;		/* IPv4, IHL 6 (20 + router alert) */
	PKT->protocol = IGMP_PROTO;
	PKT->rtl_tag.pmask = INGRESS_PORT << 8;	/* handler reads (pmask >> 8) & 0xf */
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

static void deliver(void)
{
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

	printf("\n%s (%d checks, %d failed)\n",
	       tests_failed ? "IGMP: FAILURES" : "IGMP: ALL PASS",
	       tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
