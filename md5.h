#ifndef _MD5_H_
#define _MD5_H_

#include <stdint.h>

/*
 * MD5 and HMAC-MD5 for RADIUS (dot1x.c), same bank as their caller. Operands
 * go through XDATA, see doc/internal-ram.md. HMAC keys are at most 64 bytes.
 */
extern __xdata uint8_t md5_out[16];
extern __xdata uint8_t * __xdata md5_p;
extern __xdata uint16_t md5_n;

void md5_init(void);
void md5_update(void);		/* md5_n bytes at md5_p */
void md5_final(void);		/* digest in md5_out */
void hmac_md5_init(void);	/* key: md5_n bytes at md5_p, which must stay until the final */
void hmac_md5_final(void);	/* digest in md5_out */

#define MD5_ADD(p, n)	do { md5_p = (__xdata uint8_t *)(p); md5_n = (n); md5_update(); } while (0)

#endif
