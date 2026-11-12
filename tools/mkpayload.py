#!/usr/bin/env python3
"""把编译好的 wind-<ver>.ko 加密 + base64 内嵌成 payload.c

用法:
    python3 tools/mkpayload.py dist/*.ko -o tools/flasher/jni/payload.c

加密: ChaCha20 (RFC 7539), 每条独立随机 nonce。
密钥是编译期常量, 这点必须说清楚 —— 它挡的是 strings/解包直接拿走 ko,
挡不住把二进制拖进 IDA 把 key 挖出来。真正的秘密只能来自外部输入。
"""
import argparse
import base64
import os
import re
import struct
import sys

# 32 字节主密钥。改这里要同步改 tools/flasher/jni/loader.c 里的 k_obf。
KEY = bytes([
    0x9e, 0x37, 0x79, 0xb9, 0x7f, 0x4a, 0x7c, 0x15,
    0xc3, 0x27, 0x8f, 0xd4, 0x62, 0xa1, 0x0b, 0xe6,
    0x11, 0xcd, 0x54, 0x3a, 0xf8, 0x70, 0x92, 0x1d,
    0x8b, 0x4e, 0x26, 0xd9, 0x35, 0x6f, 0xa8, 0x03,
])
# C 侧存的是 key 每 4 字节异或这个掩码
MASK = 0xA5C3E10F


# ---------- ChaCha20 ----------
def _rotl(v, c):
    return ((v << c) | (v >> (32 - c))) & 0xFFFFFFFF


def _qr(x, a, b, c, d):
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] ^= x[a]; x[d] = _rotl(x[d], 16)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] ^= x[c]; x[b] = _rotl(x[b], 12)
    x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] ^= x[a]; x[d] = _rotl(x[d], 8)
    x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] ^= x[c]; x[b] = _rotl(x[b], 7)


def _block(key, nonce, counter):
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    st += list(struct.unpack('<8I', key))
    st += [counter]
    st += list(struct.unpack('<3I', nonce))
    x = list(st)
    for _ in range(10):
        _qr(x, 0, 4, 8, 12); _qr(x, 1, 5, 9, 13)
        _qr(x, 2, 6, 10, 14); _qr(x, 3, 7, 11, 15)
        _qr(x, 0, 5, 10, 15); _qr(x, 1, 6, 11, 12)
        _qr(x, 2, 7, 8, 13); _qr(x, 3, 4, 9, 14)
    return struct.pack('<16I', *[(x[i] + st[i]) & 0xFFFFFFFF for i in range(16)])


def chacha20_xor(data, key, nonce):
    out = bytearray(data)
    off = 0
    ctr = 1
    while off < len(out):
        ks = _block(key, nonce, ctr)
        ctr += 1
        n = min(64, len(out) - off)
        for i in range(n):
            out[off + i] ^= ks[i]
        off += n
    return bytes(out)


# ---------- ko 元信息 ----------
def read_vermagic(path):
    with open(path, 'rb') as f:
        blob = f.read()
    m = re.search(rb'vermagic=([^\x00\n]+)', blob)
    if not m:
        return None
    return m.group(1).decode('utf-8', 'replace').strip()


def kernel_short(vermagic):
    """vermagic 第一段是内核版本, 如 6.6.118-android15-8-gxxxx → 6.6.118"""
    head = vermagic.split(' ')[0]
    return head.split('-')[0]


def kver_from_name(path):
    base = os.path.basename(path)
    m = re.search(r'wind-([0-9]+\.[0-9]+(\.[0-9]+)?)\.ko$', base)
    return m.group(1) if m else None


# ---------- 生成 ----------
def c_bytes(data, per_line=16, indent='\t'):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + ','.join('0x%02x' % b for b in chunk) + ',')
    return '\n'.join(lines)


def emit(entries, out_path):
    hdr = []
    hdr.append('/* 自动生成, 勿手改。源: tools/mkpayload.py */')
    hdr.append('#include <stdint.h>')
    hdr.append('#include "payload.h"')
    hdr.append('')

    body = []
    for i, e in enumerate(entries):
        body.append('static const char b64_%d[] =\n' % i)
        # base64 每 100 字符一行, 避免单行过长
        s = base64.b64encode(e['cipher']).decode()
        for j in range(0, len(s), 100):
            body.append('\t"%s"\n' % s[j:j + 100])
        body.append(';\n\n')

    rows = []
    for i, e in enumerate(entries):
        nonce = ','.join('0x%02x' % b for b in e['nonce'])
        rows.append('\t{ "%s", "%s", %du, { %s }, b64_%d },'
                    % (e['kver'], e['vermagic'].replace('"', ''), e['size'], nonce, i))

    src = '\n'.join(hdr) + '\n' + ''.join(body)
    src += 'const struct wind_payload wind_payloads[] = {\n'
    src += '\n'.join(rows) + '\n};\n\n'
    src += 'const int wind_payload_cnt = %d;\n' % len(entries)

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(src)
    return len(src)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ko', nargs='+')
    ap.add_argument('-o', '--out', default='tools/flasher/jni/payload.c')
    args = ap.parse_args()

    entries = []
    for path in args.ko:
        vm = read_vermagic(path)
        kver = kver_from_name(path) or (kernel_short(vm) if vm else None)
        if not vm or not kver:
            print('[!] 跳过 %s: 读不到 vermagic' % path, file=sys.stderr)
            continue
        plain = open(path, 'rb').read()
        nonce = os.urandom(12)
        cipher = chacha20_xor(plain, KEY, nonce)
        entries.append(dict(kver=kver, vermagic=vm, size=len(plain),
                            nonce=nonce, cipher=cipher))
        print('[OK] %-28s kver=%-12s %d 字节' % (os.path.basename(path), kver, len(plain)))

    if not entries:
        print('[x] 没有任何 ko 可用', file=sys.stderr)
        return 1

    # 同 kver 只留一条
    seen = {}
    for e in entries:
        seen[e['kver']] = e
    entries = list(seen.values())

    n = emit(entries, args.out)
    print('[OK] %s: %d 条, %d 字节' % (args.out, len(entries), n))
    return 0


if __name__ == '__main__':
    sys.exit(main())
