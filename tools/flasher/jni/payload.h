#ifndef _PAYLOAD_H_
#define _PAYLOAD_H_

#include <stdint.h>

/* 由 tools/mkpayload.py 生成, CI 每次重写。
 * 密文是 base64(chacha20(ko)), 运行时按内核版本挑一条解出来落盘。 */
struct wind_payload {
	const char   *kver;      /* 内核版本段, 如 "6.6.118" */
	const char   *vermagic;  /* 完整 vermagic, 诊断用 */
	unsigned int  size;      /* 明文长度 */
	uint8_t       nonce[12];
	const char   *b64;       /* base64(密文) */
};

extern const struct wind_payload wind_payloads[];
extern const int wind_payload_cnt;

#endif /* _PAYLOAD_H_ */
