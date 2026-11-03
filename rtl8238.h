#ifndef _RTL8238_H_
#define _RTL8238_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * The RTL8238B PSE controller of the POE-2G040210GSM runs an image that has
 * to be uploaded over I2C at every power-up. The image is the PoE controller
 * vendor's firmware and is not part of this repository: tools/add_pse_image.py
 * adds one, taken from the switch's own stock flash, to a built image at
 * PSE_IMAGE_START.
 *
 * On this branch the code banks end at BANK3 (image 0x27fff) and the web UI
 * starts at HTML_LOCATION (0x40000), so the region is free in both the
 * RTLPlayground and the mod-chip bridge images. It is below CONFIG_START, so a
 * web update or a bridge update rewrites it with the rest of the image.
 */
#define PSE_IMAGE_START	0x34000
#define PSE_IMAGE_MAX	0xc000

// Front-panel PoE ports, each on one channel of the chip at 0x20 (machine.pse_channel)
#define PSE_PORTS	4

/*
 * Per-port readings, decoded from the stock firmware's PoE pages:
 *   det, cls: register 0x0c + channel, low nibble detection (4 = valid PD,
 *             6 = open), high nibble class (0xc = class 0, 1-8 = class 1-8)
 *   on:       bit 4 + channel of register 0x10
 *   en:       PoE enabled: the channel's operating mode in 0x12 isn't 00 (off)
 *   i_raw:    low 16 bits of register 0x30 + 4 * channel, raw * 125 / 4096 mA
 *   v_raw:    high 16 bits of it, & 0x7fff, raw / 512 V
 */
struct pse_port {
	uint8_t det;
	uint8_t cls;
	uint8_t on;
	uint8_t en;
	uint16_t i_raw;
	uint16_t v_raw;
};

extern __xdata struct pse_port pse_ports[PSE_PORTS];
extern __xdata uint8_t pse_loaded;

void pse_load(void) __banked;
void pse_status(void) __banked;
bool pse_read_ports(void) __banked;
bool pse_port_set(uint8_t port, bool on) __banked;

#endif
