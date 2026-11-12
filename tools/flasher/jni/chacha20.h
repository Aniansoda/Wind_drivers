#ifndef _CHACHA20_H_
#define _CHACHA20_H_

#include <stddef.h>
#include <stdint.h>

/* RFC 7539: 256 位 key, 96 位 nonce, 32 位块计数 */
void chacha20_xor(uint8_t *data, size_t len,
		  const uint8_t key[32], const uint8_t nonce[12]);

#endif /* _CHACHA20_H_ */
