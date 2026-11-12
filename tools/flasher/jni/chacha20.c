/* ChaCha20 (RFC 7539) — 只做 XOR 流, 自带实现不依赖 openssl */
#include <string.h>
#include "chacha20.h"

#define ROTL32(v, c)  ((uint32_t)(((v) << (c)) | ((v) >> (32 - (c)))))

#define QR(a, b, c, d)                    \
	do {                              \
		a += b; d ^= a; d = ROTL32(d, 16); \
		c += d; b ^= c; b = ROTL32(b, 12); \
		a += b; d ^= a; d = ROTL32(d, 8);  \
		c += d; b ^= c; b = ROTL32(b, 7);  \
	} while (0)

static void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
			   uint32_t counter, uint8_t out[64])
{
	uint32_t st[16];
	uint32_t x[16];
	int i;

	st[0] = 0x61707865u;
	st[1] = 0x3320646eu;
	st[2] = 0x79622d32u;
	st[3] = 0x6b206574u;
	for (i = 0; i < 8; i++)
		st[4 + i] = (uint32_t)key[i * 4] |
			    ((uint32_t)key[i * 4 + 1] << 8) |
			    ((uint32_t)key[i * 4 + 2] << 16) |
			    ((uint32_t)key[i * 4 + 3] << 24);
	st[12] = counter;
	for (i = 0; i < 3; i++)
		st[13 + i] = (uint32_t)nonce[i * 4] |
			     ((uint32_t)nonce[i * 4 + 1] << 8) |
			     ((uint32_t)nonce[i * 4 + 2] << 16) |
			     ((uint32_t)nonce[i * 4 + 3] << 24);

	memcpy(x, st, sizeof(x));

	for (i = 0; i < 10; i++) {
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}

	for (i = 0; i < 16; i++) {
		uint32_t v = x[i] + st[i];
		out[i * 4]     = (uint8_t)(v);
		out[i * 4 + 1] = (uint8_t)(v >> 8);
		out[i * 4 + 2] = (uint8_t)(v >> 16);
		out[i * 4 + 3] = (uint8_t)(v >> 24);
	}
}

void chacha20_xor(uint8_t *data, size_t len,
		  const uint8_t key[32], const uint8_t nonce[12])
{
	uint8_t ks[64];
	uint32_t counter = 1;   /* 0 号块留给未来做 MAC */
	size_t off = 0;

	while (off < len) {
		size_t n = len - off;
		size_t i;

		if (n > 64)
			n = 64;
		chacha20_block(key, nonce, counter++, ks);
		for (i = 0; i < n; i++)
			data[off + i] ^= ks[i];
		off += n;
	}
	memset(ks, 0, sizeof(ks));
}
