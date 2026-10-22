#ifndef _RTL837X_IGMP_H_
#define _RTL837X_IGMP_H_

#include <stdint.h>

#define IGMP_PROTO		2
#define IGMP_QUERY		0x11
#define IGMP_V1_REPORT		0x12
#define IGMP_DVMRP		0x13
#define IGMP_PIM_V1		0x14
#define IGMP_V2_REPORT		0x16
#define IGMP_V2_LEAVE		0x17
#define IGMP_V3_REPORT		0x22
/* IGMPv3 group record types (RFC 3376) */
#define IGMP_V3_MODE_IS_INCLUDE	1
#define IGMP_V3_MODE_IS_EXCLUDE	2
#define IGMP_V3_TO_INCLUDE	3
#define IGMP_V3_TO_EXCLUDE	4
#define IGMP_V3_ALLOW_NEW	5
#define IGMP_V3_BLOCK_OLD	6
#define IGMP_LOCAL_NET_0	224

/* Seconds a port stays a router port after a query or a routing protocol
 * message arrived on it: RFC 3376's Other Querier Present Interval,
 * 2 * 125 s + 10 s / 2 */
#define IGMP_RPORT_AGE		255

/* Logical ports with a multicast router that never queries, set with
 * "igmp router"; ports that send queries are learned on top of these */
extern __xdata uint16_t igmp_rport_static;

void igmp_init(void) __banked;
void igmp_setup(void) __banked;
void igmp_enable(void) __banked;
void igmp_router_port_set(uint16_t pmask) __banked;
uint8_t igmp_packet_handler(void) __banked;
void igmp_show(void) __banked;
void igmp_tick(void) __banked;
uint16_t igmp_router_ports(void) __banked;

#endif
