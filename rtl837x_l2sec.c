/*
 * Sticky MAC addresses: on a port marked sticky, every address the switch
 * learns becomes a static entry and an "l2 static" line in the command
 * history, so saving the configuration keeps it across restarts. The commands
 * are in cmd_parser.c (parse_l2_security()); see doc/l2.md.
 *
 * The L2 table is walked a few entries per tick, so a full table takes a few
 * seconds and the main loop never stalls on it.
 * This code is in the Public Domain
 */

#pragma codeseg BANK4
#pragma constseg BANK4

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_l2sec.h"
#include "cmd_parser.h"
#include "machine.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;
extern __xdata char save_cmd;
extern __code const uint8_t * __code const hex;

// Logical ports whose learned addresses are made static
__xdata uint16_t l2_sticky_pmask;
// Table index the walk goes on from
__xdata uint16_t l2_sticky_next;

static __xdata uint8_t sticky_mac[6];
static __xdata uint16_t sticky_vlan;

#define STICKY_STEP	4

static void table_wait(void)
{
	do {
		reg_read_m(RTL837X_TBL_CTRL);
	} while (sfr_data[3] & TBL_EXECUTE);
}

static void hist_put(char c)
{
	cmd_history[cmd_history_ptr] = c;
	cmd_history_ptr = (cmd_history_ptr + 1) & CMD_HISTORY_MASK;
}

static void hist_hex(uint8_t v)
{
	hist_put(hex[v >> 4]);
	hist_put(hex[v & 0xf]);
}

static void hist_num(uint16_t v)
{
	__xdata uint8_t d[5];
	uint8_t n = 0;

	do {
		d[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		hist_put(d[--n]);
}

/*
 * "l2 static <mac> <vlan> <port>" for the entry just made static, into the
 * command history as if typed, so that saving the configuration keeps it
 */
static void sticky_save(uint8_t port)
{
	__code const char *s = "l2 static ";
	uint8_t i;

	print_string("L2: sticky ");
	for (i = 0; i < 6; i++) {
		if (i)
			write_char(':');
		print_byte(sticky_mac[i]);
	}
	print_string(" vlan ");
	itoa_short(sticky_vlan);
	print_string(" port ");
	itoa(machine.log_to_phys_port[port]);
	write_char('\n');

	if (!save_cmd)
		return;
	while (*s)
		hist_put(*s++);
	for (i = 0; i < 6; i++) {
		if (i)
			hist_put(':');
		hist_hex(sticky_mac[i]);
	}
	hist_put(' ');
	hist_num(sticky_vlan);
	hist_put(' ');
	hist_num(machine.log_to_phys_port[port]);
	hist_put('\n');
}

/*
 * Called from the main loop's tick: look at the next few unicast entries and
 * make each learned one on a sticky port static
 */
void l2_sticky_tick(void) __banked
{
	uint8_t n, port;

	if (!l2_sticky_pmask)
		return;

	for (n = 0; n < STICKY_STEP; n++) {
		table_wait();
		reg_read_m(RTL837x_TBL_DATA_0);
		REG_WRITE(RTL837x_TBL_DATA_0, sfr_data[0], sfr_data[1] & 0xf8,
			  (sfr_data[2] & 0x3f) | (TBL_LUTREAD_NEXT_L2UC << 6), sfr_data[3]);
		REG_WRITE(RTL837X_TBL_CTRL, l2_sticky_next >> 8, l2_sticky_next, TBL_L2_UNICAST, TBL_EXECUTE);
		table_wait();

		// The entry found, at or after the index asked for
		reg_read_m(RTL837x_TBL_DATA_0);
		l2_sticky_next = ((((uint16_t)sfr_data[2] & 0x0f) << 8) | sfr_data[3]) + 1;
		l2_sticky_next &= 0x0fff;

		reg_read_m(RTL837x_L2_DATA_OUT_B);
		if (!(sfr_data[0] & 0x20))	// no valid entry: the table is empty
			return;
		port = (sfr_data[0] >> 6) & 0x3;
		sticky_vlan = (((uint16_t)sfr_data[0] & 0x0f) << 8) | sfr_data[1];
		sticky_mac[0] = sfr_data[2];
		sticky_mac[1] = sfr_data[3];
		reg_read_m(RTL837x_L2_DATA_OUT_A);
		sticky_mac[2] = sfr_data[0];
		sticky_mac[3] = sfr_data[1];
		sticky_mac[4] = sfr_data[2];
		sticky_mac[5] = sfr_data[3];
		reg_read_m(RTL837x_L2_DATA_OUT_C);
		if ((sfr_data[1] & 0x01) || (sticky_mac[0] & 0x01))	// static already, or multicast
			continue;
		port |= (sfr_data[3] & 0x3) << 2;
		if (!((l2_sticky_pmask >> port) & 1))
			continue;

		// The same MAC and VLAN again replaces the learned entry (doc/l2.md)
		l2_static_port = port;
		port_l2_static(sticky_mac, sticky_vlan, false);
		sticky_save(port);
	}
}
