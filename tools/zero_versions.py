#!/usr/bin/env python3
"""走「零 CRC」路子: 把 .ko 的 __versions / __version_ext_* 段长度清零。

依据 6.12.58 真源码 (kernel/module/version.c + main.c):
  check_version():  versindex != 0 但 sh_size/sizeof(modversion_info) == 0
                    → 循环 0 次 → pr_warn_once("no symbol version for X") → return 1  (放行)
                    versindex == 0（段完全不存在）→ try_to_force_load()，要 insmod -f 才过
  same_magic():     has_crcs = (__versions 段索引 != 0) 为真时, vermagic 只比第一个空格
                    之后的尾巴(" SMP preempt mod_unload modversions aarch64"), 版本号部分不管
  elf_validity_cache_index_versions():
                    __version_ext_crcs 存在但 sh_size=0 → crc_count=0, 不做校验;
                    之后 check_version 走 ext 分支但 0 条 → 同样 pr_warn_once 放行
所以: __versions 段必须**留着但长度 0**, ext crc 段也清零, 内核就整段跳过 CRC 比对。

用法:
  python3 zero_versions.py <ko> [-o out.ko] [--dry-run]
注意:
  * 段长度清零后内核不再校验任何符号 ABI。模块代码若与目标内核结构体布局不符,
    会顺利加载然后 panic —— 这条路只适合「源码/配置本来就对」的 ko。
"""
import os
import shutil
import struct
import sys

TARGETS = ('__versions', '__version_ext_crcs', '__version_ext_names')


def is_target(name):
    # 兼容厂商把段名改成 X_version_ext_crcs 之类的写法(运行期再改回 __)
    return name == '__versions' or name.endswith('version_ext_crcs') or name.endswith('version_ext_names')


def parse(d):
    if d[:4] != b'\x7fELF' or d[4] != 2 or d[5] != 1:
        raise SystemExit('不是 ELF64 小端 .ko')
    shoff = struct.unpack_from('<Q', d, 0x28)[0]
    es = struct.unpack_from('<H', d, 0x3A)[0]
    n = struct.unpack_from('<H', d, 0x3C)[0]
    si = struct.unpack_from('<H', d, 0x3E)[0]
    so = struct.unpack_from('<Q', d, shoff + si * es + 24)[0]
    ss = struct.unpack_from('<Q', d, shoff + si * es + 32)[0]
    names = d[so:so + ss]
    secs = []
    for k in range(n):
        o = shoff + k * es
        no = struct.unpack_from('<I', d, o)[0]
        nm = names[no:names.index(b'\0', no)].decode('latin1')
        secs.append((nm, o, struct.unpack_from('<Q', d, o + 32)[0]))
    return secs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    if not args:
        print(__doc__)
        return 1
    src = args[0]
    out = src
    if '-o' in sys.argv:
        out = sys.argv[sys.argv.index('-o') + 1]
    dry = '--dry-run' in sys.argv

    d = bytearray(open(src, 'rb').read())
    secs = parse(d)
    hit = []
    for nm, hdrpos, size in secs:
        if is_target(nm):
            hit.append((nm, hdrpos, size))
    if not any(nm == '__versions' for nm, _, _ in hit):
        print('[!] 这个 ko 没有 __versions 段: 内核会走 try_to_force_load(), 必须 insmod -f 才能过。')
    print('%-24s %-10s %s' % ('段', '原长度', '处理后'))
    changed = False
    for nm, hdrpos, size in hit:
        print('%-24s %-10d %s' % (nm, size, '0 (不动)' if size == 0 else '0'))
        if size != 0:
            struct.pack_into('<Q', d, hdrpos + 32, 0)
            changed = True
    if not hit:
        print('[x] 三个段一个都没有, 没什么可清')
        return 1
    if dry:
        print('--dry-run: 不写文件')
        return 0
    if not changed:
        print('长度本来就是 0, 无需改动')
        if out != src:
            shutil.copyfile(src, out)
        return 0
    if out == src:
        shutil.copyfile(src, src + '.bak')
        print('原文件已备份: %s.bak' % src)
    open(out, 'wb').write(bytes(d))
    # 复核
    d2 = open(out, 'rb').read()
    ok = all(sz == 0 for nm, h, sz in parse(d2) if is_target(nm))
    print('写出 %s (%d 字节)  段长度复核: %s' % (out, len(d2), 'OK' if ok else '失败'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
