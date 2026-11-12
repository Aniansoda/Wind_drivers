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

/* ---- 当前内核 release (含 -android 后缀) ---- */
static int current_release(char *buf, size_t cap)
{
	FILE *f;
	const char *force = getenv("WIND_KVER_FORCE");
	char line[512];
	char *p, *e;
	size_t n;

	/* 只给测试/CI 用: 沙箱里读不到 /proc/version 时手动指定 */
	if (force && force[0]) {
		n = strlen(force);
		if (n == 0 || n >= cap)
			return -1;
		memcpy(buf, force, n);
		buf[n] = 0;
		return 0;
	}

	f = fopen("/proc/version", "r");
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
	e = strchr(p, ' ');
	n = e ? (size_t)(e - p) : strlen(p);
	if (n == 0 || n >= cap)
		return -1;
	memcpy(buf, p, n);
	buf[n] = 0;
	return 0;
}

/* "6.6.118-android15-8" -> "6";  "5.10.198" -> "5"
 * 只认大版本: 6.6.66 的 ko 也要能在 6.6.118 上被选中 */
static int major_ver(const char *in, char *out, size_t cap)
{
	size_t i = 0;

	if (!in || in[0] < '0' || in[0] > '9')
		return -1;
	while (in[i] >= '0' && in[i] <= '9')
		i++;
	if (i == 0 || i >= cap)
		return -1;
	memcpy(out, in, i);
	out[i] = 0;
	return 0;
}

/* "6.6.118" -> 118; 解析不到返回 -1 */
static long patch_level(const char *in)
{
	long v = 0;
	int seg = 0;

	if (!in)
		return -1;
	while (*in) {
		if (*in == '.') {
			if (++seg == 2) {
				while (*in >= '0' && *in <= '9') {
					v = v * 10 + (*in - '0');
					in++;
				}
				return v;
			}
			v = 0;
			in++;
			continue;
		}
		if (*in < '0' || *in > '9')
			return -1;
		in++;
	}
	return -1;
}

/* 先找版本段全等, 没有再退回大版本相同; 大版本里取 patch 最大的 */
static const struct wind_payload *pick_payload(const char *release)
{
	char want[8];
	const struct wind_payload *exact = NULL;
	const struct wind_payload *loose = NULL;
	long loose_patch = -1;
	int i;

	if (major_ver(release, want, sizeof(want)) != 0)
		return NULL;

	for (i = 0; i < wind_payload_cnt; i++) {
		char have[8];
		const struct wind_payload *p = &wind_payloads[i];

		if (!p->kver)
			continue;

		if (!strcmp(p->kver, release)) {
			exact = p;
			continue;
		}
		if (major_ver(p->kver, have, sizeof(have)) != 0)
			continue;
		if (strcmp(have, want) != 0)
			continue;
		{
			long pl = patch_level(p->kver);

			if (pl > loose_patch) {
				loose_patch = pl;
				loose = p;
			}
		}
	}
	return exact ? exact : loose;
}

/* 把 ko 里的 vermagic 版本段改成当前内核的。
 * .modinfo 是定长字符串, 只能写不长于原串的值; 短了就补 NUL 收尾。 */
static int patch_vermagic(unsigned char *buf, size_t len, const char *release)
{
	const char *tag = "vermagic=";
	unsigned char *p, *val;
	size_t taglen = strlen(tag);
	size_t toklen;
	size_t devlen;

	p = memmem(buf, len, tag, taglen);
	if (!p)
		return -1;
	val = p + taglen;

	/* 版本段 = 到空格或 NUL 为止 */
	toklen = 0;
	while (val + toklen < buf + len && val[toklen] != ' ' && val[toklen] != 0)
		toklen++;

	devlen = strlen(release);
	if (toklen == devlen && !memcmp(val, release, devlen))
		return 0;               /* 已经一致 */

	if (devlen <= toklen) {
		char old[64];
		size_t keep = toklen < sizeof(old) - 1 ? toklen : sizeof(old) - 1;

		memcpy(old, val, keep);
		old[keep] = 0;
		memcpy(val, release, devlen);
		val[devlen] = 0;        /* 短串: 截断, 后面残留不影响解析 */
		printf("[*] vermagic 改写: [%s] -> [%s]\n", old, release);
		return 1;
	}

	fprintf(stderr,
		"[!] vermagic 版本段比载荷长, 原地改不下\n"
		"    载荷 %zu 字节 [%.*s]\n"
		"    本机 %zu 字节 [%s]\n"
		"    insmod 会报 version magic 不匹配, 这版内核得单独编\n",
		toklen, (int)toklen, (char *)val, devlen, release);
	return 0;
}

int wind_payload_extract(const char *dest)
{
	char release[128];
	const struct wind_payload *hit;
	unsigned char key[32];
	unsigned char *plain = NULL;
	size_t plainlen = 0;
	int i, fd, rc = WPE_OK;
	ssize_t w;

	if (wind_payload_cnt <= 0)
		return WPE_NO_PAYLOAD;

	if (current_release(release, sizeof(release)) != 0)
		return WPE_NO_MATCH;

	hit = pick_payload(release);
	if (!hit) {
		char want[32];

		if (major_ver(release, want, sizeof(want)) != 0)
			snprintf(want, sizeof(want), "?");
		fprintf(stderr, "[x] 内嵌载荷没有 %s 的, 有:\n", want);
		for (i = 0; i < wind_payload_cnt; i++)
			fprintf(stderr, "      %s\n", wind_payloads[i].kver);
		return WPE_NO_MATCH;
	}

	if (strcmp(hit->kver, release) != 0)
		printf("[*] 大版本匹配: 本机 %s -> 用载荷 %s\n",
		       release, hit->kver);

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

	patch_vermagic(plain, plainlen, release);

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
