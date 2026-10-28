#include "machine_init_defs.h"

void machine_custom_init(void) __banked
{
	// Pad 6 drives port 6's (right SFP) LED, and pads 0-7 are off at reset
	reg_bit_set(RTL837X_REG_LED_GLB_IO_EN, 6);
}
