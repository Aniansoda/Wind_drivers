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
#include <stdint.h>

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

/* "6.6.118-android15-8" -> "6.6";  "5.10.198" -> "5.10"
 * 只比前两段: 6.6.x 的 ko 能在 6.6.y 上刷, 6.1 的不会串到 6.6 去。 */
static int major_minor(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	int dot = 0;

	if (!in)
		return -1;
	while (in[n] && n + 1 < cap) {
		if (in[n] >= '0' && in[n] <= '9') {
			out[n] = in[n];
			n++;
			continue;
		}
		if (in[n] == '.' && dot == 0) {
			out[n] = '.';
			n++;
			dot = 1;
			continue;
		}
		break;
	}
	if (n == 0)
		return -1;
	out[n] = 0;
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

/* 先找版本段全等, 再退回同版本线; 同线里取 patch 最大的那条 */
static const struct wind_payload *pick_payload(const char *release)
{
	char want[8];
	const struct wind_payload *exact = NULL;
	const struct wind_payload *loose = NULL;
	long loose_patch = -1;
	int i;

	if (major_minor(release, want, sizeof(want)) != 0)
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
		if (major_minor(p->kver, have, sizeof(have)) != 0)
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

struct modinfo_loc {
	size_t sh;      /* 段头在文件里的偏移 */
	size_t off;     /* 段内容在文件里的偏移 */
	size_t size;
};

/* 定位 .modinfo: 段头位置 + 内容范围 */
static int find_modinfo(const unsigned char *buf, size_t len, struct modinfo_loc *loc)
{
	uint64_t shoff;
	uint16_t shentsize, shnum, shstrndx;
	size_t names;
	uint64_t i;

	if (len < 64 || buf[0] != 0x7f || buf[1] != 'E' ||
	    buf[2] != 'L' || buf[3] != 'F')
		return -1;
	if (buf[4] != 2 || buf[5] != 1)         /* ELFCLASS64, LSB */
		return -1;

	memcpy(&shoff, buf + 0x28, 8);
	memcpy(&shentsize, buf + 0x3A, 2);
	memcpy(&shnum, buf + 0x3C, 2);
	memcpy(&shstrndx, buf + 0x3E, 2);
	if (shentsize < 64 || shnum == 0 || shstrndx >= shnum)
		return -1;
	if (shoff + (uint64_t)shentsize * shnum > len)
		return -1;

	{
		size_t tab = (size_t)(shoff + (uint64_t)shentsize * shstrndx);

		memcpy(&names, buf + tab + 0x18, 8);
		if (names >= len)
			return -1;
	}

	for (i = 0; i < shnum; i++) {
		size_t sh = (size_t)(shoff + i * shentsize);
		uint32_t nameoff;
		uint64_t off, size;
		const char *nm;

		memcpy(&nameoff, buf + sh, 4);
		memcpy(&off, buf + sh + 0x18, 8);
		memcpy(&size, buf + sh + 0x20, 8);
		if (names + nameoff >= len)
			continue;
		nm = (const char *)(buf + names + nameoff);
		if (strcmp(nm, ".modinfo") != 0)
			continue;
		if (off > len || off + size > len)
			return -1;
		loc->sh = sh;
		loc->off = (size_t)off;
		loc->size = (size_t)size;
		return 0;
	}
	return -1;
}

/* 把 vermagic 的版本号换成当前内核的, 后面 " SMP preempt mod_unload ..."
 * 这些 flags 原样保留 —— 内核比的就是空格之后那一段, 丢了必炸。 */
static unsigned char *fix_vermagic(unsigned char *buf, size_t *lenp, const char *release)
{
	const char *tag = "vermagic=";
	size_t taglen = strlen(tag);
	size_t devlen = strlen(release);
	struct modinfo_loc ml;
	unsigned char *val;
	const unsigned char *suf;
	char sufbuf[128];
	size_t toklen = 0, suflen, newlen;
	size_t pos, end, bn = 0, newoff, cap;
	unsigned char *blob, *nb;
	uint64_t o64, s64;

	val = memmem(buf, *lenp, tag, taglen);
	if (!val)
		return buf;
	val += taglen;

	while (val + toklen < buf + *lenp && val[toklen] != ' ' && val[toklen] != 0)
		toklen++;
	if (val + toklen >= buf + *lenp)
		return buf;

	suflen = strnlen((const char *)(val + toklen),
			 (size_t)(buf + *lenp - (val + toklen)));
	if (suflen >= sizeof(sufbuf))
		return buf;
	/* flags 先抄到本地再动 buf: 覆盖版本号会把原位置冲掉 */
	memcpy(sufbuf, val + toklen, suflen);
	sufbuf[suflen] = 0;
	suf = (const unsigned char *)sufbuf;
	newlen = devlen + suflen;

	if (toklen == devlen && !memcmp(val, release, devlen))
		return buf;                     /* 版本号本来就对 */

	/* 版本号不变长: 原地重写 版本号 + flags */
	if (devlen <= toklen) {
		memcpy(val, release, devlen);
		memcpy(val + devlen, suf, suflen);
		val[newlen] = 0;
		return buf;
	}

	if (find_modinfo(buf, *lenp, &ml) != 0)
		return buf;                     /* 不是 ELF, 动不了 */

	/* .modinfo 里后面还有空档就直接写 */
	if ((size_t)(val - buf) + newlen + 1 <= ml.off + ml.size) {
		memcpy(val, release, devlen);
		memcpy(val + devlen, suf, suflen);
		val[newlen] = 0;
		return buf;
	}

	/* 写不下: 整段 .modinfo 搬到文件尾, 段头指过去 */
	cap = ml.size + devlen + suflen + 128;
	blob = malloc(cap);
	if (!blob)
		return buf;

	pos = ml.off;
	end = ml.off + ml.size;
	while (pos < end) {
		const unsigned char *e = buf + pos;
		size_t elen = strnlen((const char *)e, end - pos);

		if (elen == 0)
			break;                  /* 后面全是填充 */
		if (!strncmp((const char *)e, tag, taglen)) {
			bn += (size_t)snprintf((char *)blob + bn, cap - bn,
					       "%s%s%s", tag, release,
					       (const char *)suf) + 1;
		} else {
			memcpy(blob + bn, e, elen);
			bn += elen;
			blob[bn++] = 0;
		}
		pos += elen + 1;
	}
	while (bn & 7)
		blob[bn++] = 0;

	newoff = (*lenp + 7) & ~(size_t)7;
	nb = realloc(buf, newoff + bn);
	if (!nb) {
		free(blob);
		return buf;
	}
	memset(nb + *lenp, 0, newoff - *lenp);
	memcpy(nb + newoff, blob, bn);
	free(blob);

	o64 = newoff;
	s64 = bn;
	memcpy(nb + ml.sh + 0x18, &o64, 8);
	memcpy(nb + ml.sh + 0x20, &s64, 8);

	*lenp = newoff + bn;
	return nb;
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

		if (major_minor(release, want, sizeof(want)) != 0)
			snprintf(want, sizeof(want), "?");
		printf("\033[31m[+] 没有 %s 的载荷, 可用: ", want);
		for (i = 0; i < wind_payload_cnt; i++)
			printf("%s ", wind_payloads[i].kver);
		printf("\033[0m\n");
		return WPE_NO_MATCH;
	}

	plain = b64_decode(hit->b64, strlen(hit->b64), &plainlen);
	if (!plain)
		return WPE_BAD_B64;
	if (plainlen != hit->size) {
		printf("[+] 解出来 %zu 字节, 头部声明 %u\n", plainlen, hit->size);
		free(plain);
		return WPE_BAD_SIZE;
	}

	key_build(key);
	chacha20_xor(plain, plainlen, key, hit->nonce);
	memset(key, 0, sizeof(key));

	/* 落盘前先认一下是不是 ELF。不是就别让它白跑一趟 insmod,
	 * 内核那边只会回一句 Exec format error, 看不出是哪一步坏的。 */
	if (plainlen < 64 || memcmp(plain, "\177ELF", 4) != 0) {
		unsigned int a = plainlen > 0 ? plain[0] : 0;
		unsigned int b = plainlen > 1 ? plain[1] : 0;
		unsigned int c = plainlen > 2 ? plain[2] : 0;
		unsigned int d = plainlen > 3 ? plain[3] : 0;

		printf("\033[31m[+]\033[0m 解出来不是 ELF: 头 %02x %02x %02x %02x, %zu 字节\n",
		       a, b, c, d, plainlen);
		memset(plain, 0, plainlen);
		free(plain);
		return WPE_BAD_SIZE;
	}
	{
		unsigned int mach = (unsigned int)plain[18] | ((unsigned int)plain[19] << 8);

		if (mach != 0xb7)
			printf("\033[33m[+]\033[0m e_machine=0x%x, 期望 0xb7 (AArch64)\n", mach);
	}

	plain = fix_vermagic(plain, &plainlen, release);
	if (!plain)
		return WPE_IO;

	fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		printf("[+] 写 %s 失败: %s\n", dest, strerror(errno));
		memset(plain, 0, plainlen);
		free(plain);
		return WPE_IO;
	}
	w = write(fd, plain, plainlen);
	close(fd);
	memset(plain, 0, plainlen);
	free(plain);

	if (w != (ssize_t)plainlen) {
		printf("[+] 写入不完整: %zd / %zu\n", w, plainlen);
		return WPE_IO;
	}

	printf("\033[36m[+]\033[0m 载荷 \033[32m%s\033[0m → %s (%zu 字节)\n",
	       hit->kver, dest, plainlen);
	return rc;
}
