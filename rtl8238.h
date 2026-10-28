#ifndef _RTL8238_H_
#define _RTL8238_H_

/*
 * The RTL8238B PSE controller of the POE-2G040210GSM runs an image that has
 * to be uploaded over I2C at every power-up. The image is not part of this
 * repository: a build made with PSE_IMAGE=<file> places it at
 * PSE_IMAGE_START, see doc/devices/2M-PCB49-V1.1.md.
 */
/*
 * The region sits right above the last code bank (BANK4 ends at image
 * 0x33fff) and below HTML_LOCATION, so it is inside the span that a web
 * update rewrites: check_and_flash_update_image() stops at CONFIG_START
 * (0x70000) to keep the saved config, so anything above that is never
 * written by an update. A sixth code bank would start at 0x34000 and
 * collide; the link line in the Makefile defines BANK1-BANK4 only.
 */
#define PSE_IMAGE_START	0x34000
#define PSE_IMAGE_MAX	0xc000

void pse_load(void) __banked;
void pse_status(void) __banked;

#endif
