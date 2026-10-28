#ifndef _RTL8238_H_
#define _RTL8238_H_

/*
 * The RTL8238B PSE controller of the POE-2G040210GSM runs an image that has
 * to be uploaded over I2C at every power-up. The image is not part of this
 * repository: a build made with PSE_IMAGE=<file> places it at
 * PSE_IMAGE_START, see doc/devices/2M-PCB49-V1.1.md.
 */
#define PSE_IMAGE_START	0x71000
#define PSE_IMAGE_MAX	0xe000

void pse_load(void) __banked;
void pse_status(void) __banked;

#endif
