#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""零 CRC 路线: 把 .ko 里的版本段长度清零

依据 (逐行读过的内核源码, 5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18 分支行为一致):

  kernel/module/version.c (6.12+) 或 kernel/module.c (5.x / 6.1)  check_version():
      if (versindex == 0)                  -> try_to_force_load(), 需要 insmod -f
      num_versions = sh_size / sizeof(struct modversion_info)
      for (i = 0; i < num_versions; i++) ... 表里找不到该符号时:
          /* Broken toolchain. Warn once, then let it go.. */
          pr_warn_once("%s: no symbol version for %s\\n", ...);
          return 1;                         -> 直接放行
      所以 sh_size = 0 => 循环 0 次 => 整段跳过 CRC 比对, 只打一次警告

  kernel/module/main.c  same_magic(modmagic, vermagic, info->index.vers):
      has_crcs 是 __versions 的段索引(不是长度)
      has_crcs 为真 -> amagic/bmagic 各自跳过第一个空格, 只比较尾巴
      => 段必须"存在", 删掉就退化成 insmod -f

  __versions             保留, sh_size 写 0
  *_version_ext_crcs     写 0 (6.12+ 的扩展表分支同样 0 条)
  *_version_ext_names    写 0 (crc / name 条目数必须配对, 否则 -ENOEXEC)

注意: 这只跳过符号 CRC, 不跳过 CFI 类型哈希与结构体布局校验
      config 与目标内核不一致时仍然会加载成功然后崩

用法:
    python3 zero_versions.py <ko> [-o 输出.ko] [-n]

    -o  另存, 默认就地改
    -n  只看不改 (dry-run)
就地改时自动留 <ko>.bak
"""

import os
import struct
import sys

TARGET_EXACT = ('__versions',)
TARGET_SUFFIX = ('version_ext_crcs', 'version_ext_names')

SHT_STRTAB = 3


def is_target(name):
    if name in TARGET_EXACT:
        return True
    return any(name.endswith(sfx) for sfx in TARGET_SUFFIX)


def sections(d):
    if d[:4] != b'\x7fELF':
        raise SystemExit('不是 ELF 文件')
    if d[4] != 2:
        raise SystemExit('只支持 ELF64 (EI_CLASS=%d)' % d[4])
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    shentsize = struct.unpack_from('<H', d, 0x3A)[0]
    shnum = struct.unpack_from('<H', d, 0x3C)[0]
    shstrndx = struct.unpack_from('<H', d, 0x3E)[0]
    if shentsize < 64 or shnum == 0:
        raise SystemExit('ELF 头异常: shentsize=%d shnum=%d' % (shentsize, shnum))
    base = shoff + shstrndx * shentsize
    stroff = struct.unpack_from('<Q', d, base + 24)[0]
    strsize = struct.unpack_from('<Q', d, base + 32)[0]
    strtab = d[stroff:stroff + strsize]
    out = []
    for i in range(shnum):
        hdr = shoff + i * shentsize
        nameoff = struct.unpack_from('<I', d, hdr)[0]
        end = strtab.find(b'\0', nameoff)
        name = strtab[nameoff:end if end >= 0 else None].decode('latin1')
        size = struct.unpack_from('<Q', d, hdr + 32)[0]
        out.append({'idx': i, 'name': name, 'hdr': hdr, 'size': size})
    return out


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__)
        return 0

    src = argv[1]
    out = src
    dry = False
    i = 2
    while i < len(argv):
        a = argv[i]
        if a in ('-n', '--dry-run'):
            dry = True
            i += 1
        elif a == '-o':
            if i + 1 >= len(argv):
                sys.stderr.write('-o 后面要跟路径\n')
                return 2
            out = argv[i + 1]
            i += 2
        else:
            sys.stderr.write('未知参数: %s\n' % a)
            return 2

    data = bytearray(open(src, 'rb').read())
    secs = sections(data)
    hit = [s for s in secs if is_target(s['name'])]
    if not hit:
        sys.stderr.write('没有 __versions 段: 这种 ko 只能 insmod -f, 不是零 CRC 路线的对象\n')
        return 1
    if not any(s['name'] == '__versions' for s in hit):
        sys.stderr.write('有扩展版本段但没有 __versions: same_magic 会退化, 不合路线要求\n')
        return 1

    print('%-24s %-10s %s' % ('段', '原长度', '处理后'))
    for s in hit:
        print('%-24s %-10d %s' % (s['name'], s['size'],
                                  '0 (dry-run)' if dry else '0'))
    if dry:
        print('dry-run, 未写文件')
        return 0

    if out == src:
        bak = src + '.bak'
        if not os.path.exists(bak):
            open(bak, 'wb').write(bytes(data))
            print('原文件已备份: %s' % bak)
    changed = 0
    for s in hit:
        if s['size'] == 0:
            continue
        struct.pack_into('<Q', data, s['hdr'] + 32, 0)
        changed += 1
    if out == src:
        with open(out, 'r+b') as f:
            f.seek(0)
            f.write(data)
    else:
        open(out, 'wb').write(bytes(data))

    chk = sections(bytearray(open(out, 'rb').read()))
    vers = {s['name']: s['size'] for s in chk if is_target(s['name'])}
    ok = all(v == 0 for v in vers.values())
    print('写出 %s (%d 字节)  改动 %d 段  段长度复核: %s'
          % (out, os.path.getsize(out), changed, 'OK' if ok else '失败'))
    print('version 类段: %s' % vers)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
