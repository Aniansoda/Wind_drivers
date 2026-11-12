/* 内嵌载荷解包: base64 -> ChaCha20 -> 落盘
 *
 * 密钥是编译期常量, 只能挡 strings / 直接解包拿走 ko。
 * 谁把二进制拖进 IDA, key 一样挖得出来 —— 真正的秘密得来自外部输入。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "loader.h"
#include "payload.h"
#include "chacha20.h"

/* key 每 4 字节小端异或 0xA5C3E10F, 运行时还原 */
#define KEY_MASK 0xA5C3E10Fu

static const unsigned int k_obf[8] = {
	0x1cbad691u, 0xb0bfab70u, 0x714cc6ccu, 0x43c8406du,
	0x9f972c1eu, 0xb85191f7u, 0x7ce5af84u, 0xa66b8e3au
};

static void key_build(unsigned char key[32])
{
	int i;

	for (i = 0; i < 8; i++) {
		unsigned int w = k_obf[i] ^ KEY_MASK;

		key[i * 4]     = (unsigned char)(w);
		key[i * 4 + 1] = (unsigned char)(w >> 8);
		key[i * 4 + 2] = (unsigned char)(w >> 16);
		key[i * 4 + 3] = (unsigned char)(w >> 24);
	}
}

/* ---- base64 ---- */
static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static unsigned char *b64_decode(const char *in, size_t inlen, size_t *outlen)
{
	unsigned char *out;
	size_t cap = inlen / 4 * 3 + 4;
	size_t o = 0;
	unsigned int acc = 0;
	int bits = 0;
	size_t i;

	out = malloc(cap);
	if (!out)
		return NULL;

	for (i = 0; i < inlen; i++) {
		int v = b64_val((unsigned char)in[i]);

		if (v < 0) {
			if (in[i] == '=')
				break;
			continue;   /* 换行之类直接跳过 */
		}
		acc = (acc << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= cap)
				break;
			out[o++] = (unsigned char)((acc >> bits) & 0xFF);
		}
	}
	*outlen = o;
	return out;
}

/* ---- 当前内核版本段 ---- */
static int current_kver(char *buf, size_t cap)
{
	FILE *f;
	const char *force = getenv("WIND_KVER_FORCE");

	/* 只给测试/CI 用: 沙箱里读不到 /proc/version 时手动指定 */
	if (force && force[0]) {
		size_t n = strlen(force);

		if (n == 0 || n >= cap)
			return -1;
		memcpy(buf, force, n);
		buf[n] = 0;
		return 0;
	}

	f = fopen("/proc/version", "r");
	char line[512];
	char *p, *d;
	size_t n;

	if (!f)
		return -1;
	if (!fgets(line, (int)sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);

	/* "Linux version 6.6.118-android15-8-gxxx ..." */
	p = strstr(line, "version ");
	if (!p)
		return -1;
	p += 8;
	d = strchr(p, '-');
	if (!d)
		d = strchr(p, ' ');
	n = d ? (size_t)(d - p) : strlen(p);
	if (n == 0 || n >= cap)
		return -1;
	memcpy(buf, p, n);
	buf[n] = 0;
	return 0;
}

/* kver "6.6.118" 与 payload kver 比较:
 * 完全相等, 或 payload 写的更短且是前缀 (6.6 对 6.6.118) */
static int kver_match(const char *want, const char *have)
{
	size_t n;

	if (!want || !have)
		return 0;
	n = strlen(want);
	if (!n)
		return 0;
	if (strncmp(want, have, n) != 0)
		return 0;
	return have[n] == 0 || have[n] == '.';
}

int wind_payload_extract(const char *dest)
{
	char kver[64];
	const struct wind_payload *hit = NULL;
	unsigned char key[32];
	unsigned char *plain = NULL;
	size_t plainlen = 0;
	int i, fd, rc = WPE_OK;
	ssize_t w;

	if (wind_payload_cnt <= 0)
		return WPE_NO_PAYLOAD;

	if (current_kver(kver, sizeof(kver)) != 0)
		return WPE_NO_MATCH;

	for (i = 0; i < wind_payload_cnt; i++) {
		if (kver_match(wind_payloads[i].kver, kver)) {
			hit = &wind_payloads[i];
			break;
		}
	}
	if (!hit) {
		fprintf(stderr, "[x] 内嵌载荷没有匹配 %s 的, 有:\n", kver);
		for (i = 0; i < wind_payload_cnt; i++)
			fprintf(stderr, "      %s\n", wind_payloads[i].kver);
		return WPE_NO_MATCH;
	}

	plain = b64_decode(hit->b64, strlen(hit->b64), &plainlen);
	if (!plain)
		return WPE_BAD_B64;
	if (plainlen != hit->size) {
		fprintf(stderr, "[x] 解出来 %zu 字节, 头部声明 %u\n",
			plainlen, hit->size);
		free(plain);
		return WPE_BAD_SIZE;
	}

	key_build(key);
	chacha20_xor(plain, plainlen, key, hit->nonce);
	memset(key, 0, sizeof(key));

	fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		fprintf(stderr, "[x] 写 %s 失败: %s\n", dest, strerror(errno));
		memset(plain, 0, plainlen);
		free(plain);
		return WPE_IO;
	}
	w = write(fd, plain, plainlen);
	close(fd);
	memset(plain, 0, plainlen);
	free(plain);

	if (w != (ssize_t)plainlen) {
		fprintf(stderr, "[x] 写入不完整: %zd / %zu\n", w, plainlen);
		return WPE_IO;
	}

	printf("[+] 载荷 %s → %s (%zu 字节)\n", hit->kver, dest, plainlen);
	printf("    vermagic: %s\n", hit->vermagic);
	return rc;
}
