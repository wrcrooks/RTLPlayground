#ifndef _RTL837X_L2SEC_H_
#define _RTL837X_L2SEC_H_

#include <stdint.h>

/*
 * Learning limits and sticky MAC addresses, see rtl837x_l2sec.c and the
 * l2 commands in cmd_parser.c
 */

// The learning limit RTLPlayground sets at boot, in effect none
#define L2_LEARN_UNLIMITED	0x1040

extern __xdata uint16_t l2_sticky_pmask;
extern __xdata uint16_t l2_sticky_next;

void l2_sticky_tick(void) __banked;

#endif
