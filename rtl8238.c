/*
 * Loader for the RTL8238B PSE controller on the POE-2G040210GSM
 * (StandardPOE8-V1.0 daughterboard, I2C addresses 0x20 and 0x21).
 *
 * The chip does nothing until an image is uploaded to it, which the stock
 * firmware does at every boot. This follows the stock sequence: reset through
 * register 0x1a, set up registers 0xec and 0x18, write the whole image to
 * register 0xf4 in one transfer, and check bit 5 of the status register 0xec.
 * See doc/devices/2M-PCB49-V1.1.md.
 *
 * The RTL8372's I2C master moves at most 16 bytes per transfer, so this uses
 * a bit-banged master on GPIO46 (SCL) and GPIO47 (SDA), like the stock
 * firmware. The lines are driven open drain: the output latches stay 0 and
 * only the direction bits change.
 *
 * Image region at PSE_IMAGE_START, added by the build with PSE_IMAGE=<file>:
 *   "PSE1", length of image 0, length of image 1 (both uint32, little endian),
 *   image 0, image 1.
 * Image 1 is used when register 0xe8 reads 2 after the reset, image 0
 * otherwise, as in the stock firmware. A length of 0 means "not present".
 * This code is in the Public Domain
 */

#pragma codeseg BANK4
#pragma constseg BANK4

#include <stdint.h>
#include <stdbool.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_pins.h"
#include "rtl837x_flash.h"
#include "rtl8238.h"

extern __xdata uint8_t sfr_data[4];
extern __xdata struct flash_region_t flash_region;
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];

#define PSE_ADDR_0	0x20
#define PSE_ADDR_1	0x21
#define PSE_HDR_LEN	12

// GPIO46 and GPIO47 are bits 14 and 15 of the GPIO 32-63 registers, which
// are bits 6 and 7 of sfr_data[2]
#define SCL_BIT		0x40
#define SDA_BIT		0x80

#define I2C_RETRIES	10

static __xdata uint8_t gpio_dir[4];
static __xdata uint8_t i2c_low;
static __xdata uint8_t pse_buf[4];
// 0 = ok, 1 = device/register byte, 2-5 = data byte 1-4
static __xdata uint8_t pse_fail;
static __xdata uint32_t img_len[2];

static void i2c_wait(void)
{
	__xdata volatile uint8_t i;

	for (i = 0; i < 4; i++)
		;
}

// Drive the lines in i2c_low low and release the others
static void i2c_lines(void)
{
	sfr_data[0] = gpio_dir[0];
	sfr_data[1] = gpio_dir[1];
	sfr_data[2] = (gpio_dir[2] & ~(SCL_BIT | SDA_BIT)) | i2c_low;
	sfr_data[3] = gpio_dir[3];
	reg_write_m(RTL837X_REG_GPIO_32_63_DIRECTION);
	i2c_wait();
}

static void scl_low(void)
{
	i2c_low |= SCL_BIT;
	i2c_lines();
}

static void scl_high(void)
{
	__xdata uint16_t t = 1000;

	i2c_low &= ~SCL_BIT;
	i2c_lines();
	// The slave may stretch the clock
	while (!gpio_pin_test(GPIO46_I2C_SCL0) && --t)
		;
}

static void sda_set(uint8_t v)
{
	if (v)
		i2c_low &= ~SDA_BIT;
	else
		i2c_low |= SDA_BIT;
	i2c_lines();
}

// A start condition, also used as the repeated start
static void i2c_start(void)
{
	sda_set(1);
	scl_high();
	sda_set(0);
	scl_low();
}

static void i2c_stop(void)
{
	sda_set(0);
	scl_high();
	sda_set(1);
}

// Returns true if the slave acknowledged the byte
static bool i2c_write_byte(uint8_t b)
{
	__xdata uint8_t i;
	__xdata bool ack;

	for (i = 0; i < 8; i++) {
		sda_set(b & 0x80);
		b <<= 1;
		scl_high();
		scl_low();
	}
	sda_set(1);
	scl_high();
	ack = !gpio_pin_test(GPIO47_I2C_SDA0);
	scl_low();
	return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
	__xdata uint8_t i;
	__xdata uint8_t b = 0;

	sda_set(1);
	for (i = 0; i < 8; i++) {
		scl_high();
		b <<= 1;
		if (gpio_pin_test(GPIO47_I2C_SDA0))
			b |= 1;
		scl_low();
	}
	sda_set(!ack);
	scl_high();
	scl_low();
	sda_set(1);
	return b;
}

static void i2c_setup(void)
{
	gpio_input_setup(GPIO46_I2C_SCL0);
	gpio_input_setup(GPIO47_I2C_SDA0);
	reg_bit_clear(RTL837X_REG_GPIO_32_63_OUTPUT, 14);
	reg_bit_clear(RTL837X_REG_GPIO_32_63_OUTPUT, 15);
	reg_read_m(RTL837X_REG_GPIO_32_63_DIRECTION);
	gpio_dir[0] = sfr_data[0];
	gpio_dir[1] = sfr_data[1];
	gpio_dir[2] = sfr_data[2];
	gpio_dir[3] = sfr_data[3];
	i2c_low = 0;
	i2c_lines();
}

// Start a transfer and send the device and register address, with the stock
// firmware's retries. Returns false if the device doesn't answer.
static bool pse_begin(uint8_t dev, uint8_t reg)
{
	__xdata uint8_t i;

	for (i = 0; i < I2C_RETRIES; i++) {
		i2c_start();
		if (i2c_write_byte(dev << 1) && i2c_write_byte(reg))
			return true;
		i2c_stop();
	}
	return false;
}

static void pse_set(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	pse_buf[0] = a;
	pse_buf[1] = b;
	pse_buf[2] = c;
	pse_buf[3] = d;
}

// Write the 4 bytes in pse_buf to a register, in that order
static bool pse_write(uint8_t dev, uint8_t reg)
{
	__xdata uint8_t i;

	if (!pse_begin(dev, reg)) {
		pse_fail = 1;
		return false;
	}
	for (i = 0; i < 4; i++) {
		if (!i2c_write_byte(pse_buf[i])) {
			pse_fail = i + 2;
			i2c_stop();
			return false;
		}
	}
	i2c_stop();
	pse_fail = 0;
	return true;
}

// Read 4 bytes from a register into pse_buf, in the order received. The chip
// sends the least significant byte first, so the status bits are in pse_buf[0].
static bool pse_read(uint8_t dev, uint8_t reg)
{
	__xdata uint8_t i;

	if (!pse_begin(dev, reg))
		return false;
	i2c_start();
	if (!i2c_write_byte((dev << 1) | 1)) {
		i2c_stop();
		return false;
	}
	for (i = 0; i < 4; i++)
		pse_buf[i] = i2c_read_byte(i < 3);
	i2c_stop();
	return true;
}

static void pse_print_buf(void)
{
	__xdata uint8_t i;

	for (i = 0; i < 4; i++) {
		write_char(' ');
		print_byte(pse_buf[i]);
	}
}

static uint32_t le32(__xdata uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool pse_image_header(void)
{
	flash_region.addr = PSE_IMAGE_START;
	flash_region.len = PSE_HDR_LEN;
	flash_read_bulk(flash_buf);
	if (flash_buf[0] != 'P' || flash_buf[1] != 'S' || flash_buf[2] != 'E' || flash_buf[3] != '1')
		return false;
	img_len[0] = le32(flash_buf + 4);
	img_len[1] = le32(flash_buf + 8);
	if (img_len[0] > PSE_IMAGE_MAX || img_len[1] > PSE_IMAGE_MAX
	    || PSE_HDR_LEN + img_len[0] + img_len[1] > PSE_IMAGE_MAX)
		return false;
	return img_len[0] || img_len[1];
}

// Send an image from flash to register 0xf4 as one transfer. Like the stock
// firmware, a data byte without an acknowledge doesn't stop the transfer;
// returns the number of such bytes, or 0xffff if the device didn't answer.
static uint16_t pse_write_image(uint32_t addr, uint32_t len)
{
	__xdata uint32_t a = addr;
	__xdata uint32_t left = len;
	__xdata uint16_t n, i;
	__xdata uint16_t nacks = 0;

	if (!pse_begin(PSE_ADDR_0, 0xf4))
		return 0xffff;
	while (left) {
		n = left > FLASH_BUF_SIZE ? FLASH_BUF_SIZE : left;
		flash_region.addr = a;
		flash_region.len = n;
		flash_read_bulk(flash_buf);
		for (i = 0; i < n; i++) {
			if (!i2c_write_byte(flash_buf[i]) && nacks != 0xfffe)
				nacks++;
		}
		a += n;
		left -= n;
	}
	i2c_stop();
	return nacks;
}

void pse_load(void) __banked
{
	__xdata uint8_t img;
	__xdata uint32_t addr;
	__xdata uint16_t nacks;

	if (!pse_image_header()) {
		print_string("PSE: no RTL8238B image in this build, PoE stays off\n");
		return;
	}
	i2c_setup();

	// Step 0 of the stock init: reset the whole chip. The stock firmware
	// stores the payload as a 32-bit 0x20, which Keil C51 lays out most
	// significant byte first, so it goes on the wire as 00 00 00 20.
	pse_set(0x00, 0x00, 0x00, 0x20);
	if (!pse_write(PSE_ADDR_0, 0x1a)) {
		print_string("PSE: reset write to 0x1a refused at byte ");
		print_byte(pse_fail); write_char('\n');
		return;
	}
	delay(100);

	pse_set(0x03, 0x00, 0x00, 0x00);
	if (!pse_write(PSE_ADDR_0, 0xec)) {
		print_string("PSE: write to 0xec refused at byte ");
		print_byte(pse_fail); write_char('\n');
		return;
	}
	delay(1);
	pse_read(PSE_ADDR_0, 0xec);
	print_string("PSE: status before load:"); pse_print_buf(); write_char('\n');
	if (!(pse_buf[0] & 0x04)) {
		pse_set(0x00, 0x00, 0x20, 0x39);
		pse_write(PSE_ADDR_0, 0x18);
		delay(10);
		pse_set(0x03, 0x00, 0x00, 0x00);
		pse_write(PSE_ADDR_0, 0xec);
	}

	pse_read(PSE_ADDR_0, 0xe8);
	img = (pse_buf[0] == 2 && !pse_buf[1] && !pse_buf[2] && !pse_buf[3]) ? 1 : 0;
	if (!img_len[img]) {
		print_string("PSE: this build has no image for chip type");
		pse_print_buf(); write_char('\n');
		return;
	}
	addr = PSE_IMAGE_START + PSE_HDR_LEN;
	if (img)
		addr += img_len[0];

	print_string("PSE: loading image "); write_char('0' + img);
	print_string(", "); print_long(img_len[img]); print_string(" bytes\n");
	nacks = pse_write_image(addr, img_len[img]);
	if (nacks == 0xffff) {
		print_string("PSE: load failed, no answer\n");
		return;
	}
	if (nacks) {
		print_string("PSE: bytes not acknowledged: "); print_short(nacks); write_char('\n');
	}

	delay(10);
	pse_read(PSE_ADDR_0, 0xec);
	print_string("PSE: status after load:"); pse_print_buf();
	print_string(pse_buf[0] & 0x20 ? " - load ok\n" : " - load failed\n");
	delay(10);
}

static void pse_status_dev(uint8_t dev)
{
	__code static const uint8_t regs[] = { 0xec, 0xe8, 0x18, 0x10 };
	__xdata uint8_t i;

	for (i = 0; i < sizeof(regs); i++) {
		print_string("  0x"); print_byte(dev);
		print_string(" reg 0x"); print_byte(regs[i]); write_char(':');
		if (pse_read(dev, regs[i]))
			pse_print_buf();
		else
			print_string(" no answer");
		write_char('\n');
	}
}

void pse_status(void) __banked
{
	i2c_setup();
	print_string("RTL8238B registers:\n");
	pse_status_dev(PSE_ADDR_0);
	pse_status_dev(PSE_ADDR_1);
	if (pse_image_header()) {
		print_string("Image in flash: "); print_long(img_len[0]);
		print_string(" and "); print_long(img_len[1]); print_string(" bytes\n");
	} else {
		print_string("No image in flash\n");
	}
}
