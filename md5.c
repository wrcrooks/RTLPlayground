/*
 * MD5 (RFC 1321) and HMAC-MD5 (RFC 2104), for RADIUS only: its authenticators
 * and hidden passwords are defined on them. Not for anything else.
 */

#include "rtl837x_common.h"
#include "md5.h"

#pragma codeseg BANK4
#pragma constseg BANK4

static __code const uint32_t md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
/* rotations, four per round, as whole bytes in the high nibble and bits left in the low one */
static __code const uint8_t md5_r[16] = {
	0x07, 0x14, 0x21, 0x26, 0x05, 0x11, 0x16, 0x24, 0x04, 0x13, 0x20, 0x27, 0x06, 0x12, 0x17, 0x25,
};

__xdata uint8_t md5_out[16];
__xdata uint8_t * __xdata md5_p;
__xdata uint16_t md5_n;

static __xdata uint32_t md5_h[4];
static __xdata uint32_t md5_len;
static __xdata uint8_t md5_have;
static __xdata union {
	uint8_t b[64];
	uint32_t w[16];		/* the 8051 and SDCC are little-endian, as MD5 wants */
} md5_m;
static __xdata uint32_t md5_a, md5_b, md5_c, md5_d, md5_f;
static __xdata uint8_t md5_i, md5_g, md5_rot, md5_t;
static __xdata uint8_t md5_key[64], md5_kn;


static void md5_mix(void) __reentrant
{
	if (md5_i < 16) {
		md5_f = md5_d ^ (md5_b & (md5_c ^ md5_d));
		md5_g = md5_i;
	} else if (md5_i < 32) {
		md5_f = md5_c ^ (md5_d & (md5_b ^ md5_c));
		md5_g = (5 * md5_i + 1) & 15;
	} else if (md5_i < 48) {
		md5_f = md5_b ^ md5_c ^ md5_d;
		md5_g = (3 * md5_i + 5) & 15;
	} else {
		md5_f = md5_c ^ (md5_b | ~md5_d);
		md5_g = (7 * md5_i) & 15;
	}
}


static void md5_rotate(void) __reentrant
{
	__xdata uint8_t *f = (__xdata uint8_t *)&md5_f;

	md5_rot = md5_r[((md5_i >> 2) & 0xc) | (md5_i & 3)];
	while (md5_rot & 0xf0) {
		md5_t = f[3];
		f[3] = f[2];
		f[2] = f[1];
		f[1] = f[0];
		f[0] = md5_t;
		md5_rot -= 0x10;
	}
	while (md5_rot) {
		md5_t = f[3] >> 7;
		md5_f <<= 1;
		f[0] |= md5_t;
		md5_rot--;
	}
}


static void md5_block(void) __reentrant
{
	md5_a = md5_h[0];
	md5_b = md5_h[1];
	md5_c = md5_h[2];
	md5_d = md5_h[3];
	for (md5_i = 0; md5_i < 64; md5_i++) {
		md5_mix();
		md5_f += md5_a;
		md5_f += md5_k[md5_i];
		md5_f += md5_m.w[md5_g];
		md5_a = md5_d;
		md5_d = md5_c;
		md5_c = md5_b;
		md5_rotate();
		md5_b += md5_f;
	}
	md5_h[0] += md5_a;
	md5_h[1] += md5_b;
	md5_h[2] += md5_c;
	md5_h[3] += md5_d;
	md5_have = 0;
}


void md5_init(void)
{
	md5_h[0] = 0x67452301;
	md5_h[1] = 0xefcdab89;
	md5_h[2] = 0x98badcfe;
	md5_h[3] = 0x10325476;
	md5_len = 0;
	md5_have = 0;
}


void md5_update(void)
{
	md5_len += md5_n;
	while (md5_n) {
		md5_m.b[md5_have++] = *md5_p++;
		md5_n--;
		if (md5_have == 64)
			md5_block();
	}
}


void md5_final(void)
{
	md5_len <<= 3;
	md5_m.b[md5_have++] = 0x80;
	if (md5_have > 56) {
		while (md5_have < 64)
			md5_m.b[md5_have++] = 0;
		md5_block();
	}
	while (md5_have < 64)
		md5_m.b[md5_have++] = 0;
	md5_m.w[14] = md5_len;
	md5_block();
	memcpy(md5_out, (__xdata uint8_t *)md5_h, 16);
}


static void hmac_pad(uint8_t x)
{
	static __xdata uint8_t i, pad;

	pad = x;
	md5_init();
	for (i = 0; i < 64; i++)
		md5_key[i] = (i < md5_kn ? md5_p[i] : 0) ^ pad;
	MD5_ADD(md5_key, 64);
}


static __xdata uint8_t * __xdata hmac_k;

void hmac_md5_init(void)
{
	hmac_k = md5_p;
	md5_kn = md5_n;
	hmac_pad(0x36);
}


void hmac_md5_final(void)
{
	md5_final();
	md5_p = hmac_k;
	hmac_pad(0x5c);
	MD5_ADD(md5_out, 16);
	md5_final();
}
