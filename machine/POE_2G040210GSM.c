#include "machine_defs.h"

/*
 * MokerLink POE-2G040210GSM, PCB 2M-PCB49-V1.1: 4 x 2.5G PoE + 2 x 10G SFP+.
 *
 * NOT VERIFIED ON HARDWARE. A starting point copied from the non-PoE
 * 2G040210GSM (2M-PCB43-V1.1, KP_9000_6XHML_X2_V1_1), which uses the same
 * RTL8372 4+2 design. The port mapping, SFP pins, reset button and LEDs
 * all still have to be confirmed on this board: see
 * doc/devices/2M-PCB49-V1.1.md for what is known and what is open.
 * PoE is handled by the board itself and isn't configured here.
 */
__code const struct machine machine = {
	MACHINE_COMMON_SETTING,
	.n_sfp = 2,
	.log_to_phys_port = {0, 0, 0, 5, 1, 2, 3, 4, 6},
	.phys_to_log_port = {4, 5, 6, 7, 3, 8, 0, 0, 0},
	.is_sfp = {0, 0, 0, 1, 0, 0, 0, 0, 2},
	// Left SFP port (5): unverified. The stock firmware manages PoE through
	// U16, probably over UART1 (GPIO50/51), so GPIO50 is likely wrong here.
	.sfp_port[0].pin_detect = GPIO50_I2C_SCL2_UART1_TX,
	.sfp_port[0].pin_los = GPIO10_LED10,
	.sfp_port[0].pin_tx_disable = GPIO_NA,
	.sfp_port[0].sds = 0,
	.sfp_port[0].i2c = I2CBUS( GPIO41_I2C_SDA3_MDIO1, GPIO40_I2C_SCL3_MDC1 ),
	// Right SFP port (6): unverified
	.sfp_port[1].pin_detect = GPIO30_ACL_BIT3_EN,
	.sfp_port[1].pin_los = GPIO37,
	.sfp_port[1].pin_tx_disable = GPIO_NA,
	.sfp_port[1].sds = 1,
	.sfp_port[1].i2c = I2CBUS( GPIO39_I2C_SDA4, GPIO40_I2C_SCL3_MDC1 ),
	// Not known whether this board has a reset button, nor on which pin
	.reset_pin = GPIO_NA,
	.high_leds = { .mux = LED_27 | LED_29, .enable = LED_28_SYS | LED_29 },
	.port_led_set = { 0, 0, 0, 0, 0, 0, 0, 0, 0},
	// The 2M-PCB43-V1.1's LED sets: unverified on this board
	.led_sets = { { LEDS_2G5 | LEDS_1G | LEDS_100M | LEDS_10M | LEDS_LINK | LEDS_ACT | LEDS_10G,
			LEDS_2G5 | LEDS_LINK | LEDS_10G,
			LEDS_1G | LEDS_LINK,
			0 },
		    },
};
