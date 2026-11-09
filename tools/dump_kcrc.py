#!/usr/bin/env python3
"""从设备上现成的 .ko 里抽出 __versions 的 (symbol, crc)，生成 Module.symvers 行。

内核开了 CONFIG_MODVERSIONS 时，每个 .ko 都带 __versions 段，里面是
struct modversion_info { unsigned long crc; char name[MODULE_NAME_LEN]; }
（arm64 上 8 + 56 = 64 字节一条）。这些 CRC 就是**设备内核**的 __kcrctab 值，
比任何源码侧 genksyms 结果都准，尤其 module_layout 这种随配置变化的符号。

用法:
  python3 dump_kcrc.py dump  <ko...>                  # 每个 ko 打印 symvers 行
  python3 dump_kcrc.py harvest <out.symvers> <ko...>  # 汇总去重写文件
  python3 dump_kcrc.py merge <self.symvers> <dev.symvers> <out.symvers>
  python3 dump_kcrc.py check <ko> <dev.symvers>  # 装机前预检：这个 ko 在目标机上会不会 crc 不符
  python3 dump_kcrc.py diff  <a.symvers> <b.symvers>  # 两套构建的 crc 表逐符号比：判断是不是同一套源码/config
  python3 dump_kcrc.py count <a.symvers> <b.symvers>  # 只输出 "不一致数 交集数"，CI 硬门槛用
"""
import struct
import sys
import os
import glob

ENTRY = 64          # arm64: 8 (crc) + 56 (name)
NAME_MAX = 56


def norm(v):
    """统一 crc 表示：去掉 0x、补 8 位小写。"""
    v = v.strip().lower()
    if v.startswith('0x'):
        v = v[2:]
    return v.zfill(8)


def read_versions(path):
    """返回 [(crc, name)]；没有 __versions 段返回 []。"""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        return []
    ei_class, ei_data = data[4], data[5]
    if ei_class != 2 or ei_data != 1:
        return []  # 只要 ELF64 LE（arm64）
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)
    if e_shoff == 0 or e_shnum == 0:
        return []
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<IIQQQQ', data, off)
        sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from('<IIQQ', data, off + 40)
        secs.append(dict(name_off=sh_name, type=sh_type, offset=sh_offset, size=sh_size, entsize=sh_entsize))
    if e_shstrndx >= len(secs):
        return []
    stroff = secs[e_shstrndx]['offset']
    for s in secs:
        end = data.find(b'\0', stroff + s['name_off'])
        s['name'] = data[stroff + s['name_off']:end].decode('latin-1')
    out = []
    for s in secs:
        if s['name'] not in ('__versions', '.gnu.linkonce.this_module'):
            continue
        if s['name'] != '__versions':
            continue
        step = s['entsize'] or ENTRY
        if step < 16:
            step = ENTRY
        n = s['size'] // step
        for i in range(n):
            base = s['offset'] + i * step
            if base + step > len(data):
                break
            crc, = struct.unpack_from('<Q', data, base)
            raw = data[base + 8: base + step]
            name = raw.split(b'\0', 1)[0].decode('latin-1')
            if not name:
                continue
            out.append((crc & 0xFFFFFFFF, name))
    return out


def cmd_dump(paths):
    for p in paths:
        for crc, name in read_versions(p):
            print("0x%08x\t%s\tvmlinux\tEXPORT_SYMBOL" % (crc, name))


def cmd_harvest(out_path, paths):
    table = {}
    conflict = 0
    src = {}
    for p in paths:
        for crc, name in read_versions(p):
            if name in table and table[name] != crc:
                conflict += 1
                src.setdefault(name, []).append((crc, os.path.basename(p)))
                continue
            table[name] = crc
            src.setdefault(name, []).append((crc, os.path.basename(p)))
    with open(out_path, 'w') as f:
        for name in sorted(table):
            f.write("0x%08x\t%s\tvmlinux\tEXPORT_SYMBOL\n" % (table[name], name))
    print("解析 %d 个 ko，符号 %d 个，写 %s" % (len(paths), len(table), out_path))
    if conflict:
        print("!! %d 处同名不同 crc（取第一个，下面列前 10 个）" % conflict)
        shown = 0
        for name, lst in src.items():
            if len({c for c, _ in lst}) > 1:
                print("   %s -> %s" % (name, ", ".join("0x%08x(%s)" % (c, k) for c, k in lst[:4])))
                shown += 1
                if shown >= 10:
                    break
    if 'module_layout' in table:
        print("module_layout = 0x%08x  <- 关键" % table['module_layout'])
    else:
        print("!! 没抽到 module_layout，这批 ko 不对劲")


def cmd_count(a_path, b_path):
    """只输出一个数字: 交集里 crc 不一致的个数。CI 用来做硬门槛。"""
    a, _ = load_symvers(a_path)
    b, _ = load_symvers(b_path)
    bad = sum(1 for n in a if n in b and a[n] != b[n])
    common = sum(1 for n in a if n in b)
    print("%d %d" % (bad, common))
    return 0


def cmd_merge(self_path, dev_path, out_path):
    dev = {}
    for line in open(dev_path):
        parts = line.split()
        if len(parts) >= 2:
            dev[parts[1]] = norm(parts[0])
    selfmap = {}
    order = []
    for line in open(self_path):
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[1]
        if name not in selfmap:
            order.append(name)
        selfmap[name] = parts[0]
    replaced, kept = [], []
    with open(out_path, 'w') as f:
        for name in order:
            crc = selfmap[name]
            if name in dev:
                if norm(crc) != dev[name]:
                    replaced.append((name, "0x" + norm(crc), "0x" + dev[name]))
                crc = "0x" + dev[name]
            else:
                kept.append(name)
            f.write("%s\t%s\tvmlinux\tEXPORT_SYMBOL\n" % (crc, name))
        extra = [n for n in dev if n not in selfmap]
        for name in extra:
            f.write("0x%s\t%s\tvmlinux\tEXPORT_SYMBOL\n" % (dev[name], name))
    print("自建 %d 个符号，设备表 %d 个" % (len(order), len(dev)))
    print("用设备 crc 覆盖 %d 个，其中 module_layout: %s" % (
        len(replaced), "是" if any(n == 'module_layout' for n, _, _ in replaced) else "否"))
    for name, a, b in replaced[:20]:
        print("   %-32s 自建 %s -> 设备 %s" % (name, a, b))
    missing = [n for n in order if n not in dev]
    print("设备表里没有、沿用自建 crc 的符号 %d 个（前 10）: %s" % (len(missing), ", ".join(missing[:10])))
    print("写 %s" % out_path)


def cmd_check(ko, dev_path):
    """拿 ko 自己的 __versions 对设备表，预报 insmod 会怎么死。"""
    dev = {}
    for line in open(dev_path):
        parts = line.split()
        if len(parts) >= 2:
            dev[parts[1]] = norm(parts[0])
    vers = read_versions(ko)
    if not vers:
        print("!! %s 里没有 __versions（可能没开 MODVERSIONS）" % ko)
        return 2
    bad, gone, ok = [], [], 0
    for crc, name in vers:
        want = "%08x" % crc
        if name not in dev:
            gone.append(name)
        elif dev[name] != want:
            bad.append((name, want, dev[name]))
        else:
            ok += 1
    print("%s: %d 个符号，匹配 %d" % (ko, len(vers), ok))
    if bad:
        print("!! crc 不符 %d 个（设备内核这些符号的版本不同）:" % len(bad))
        for name, mine, theirs in bad[:30]:
            print("   %-34s ko 0x%s != 设备 0x%s" % (name, mine, theirs))
    if gone:
        print("!! 设备表里没有 %d 个（设备内核可能根本没导出）: %s" % (len(gone), ", ".join(gone[:20])))
    if not bad and not gone:
        print("可以加载：__versions 全部与设备内核一致")
    return 0 if not bad and not gone else 1



def load_symvers(path):
    """读 symvers → {name: crc8}，保持文件里的出现顺序。"""
    table, order = {}, []
    for line in open(path):
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[1]
        if name not in table:
            order.append(name)
        table[name] = norm(parts[0])
    return table, order


def cmd_diff(a_path, b_path):
    """比两套构建的 crc 表。

    用来回答「哪个 tag 编出来的 crc 与设备一致」：把设备 harvest 出来的
    symvers 当 b，把某个 tag 编出来的 Module.symvers 当 a，跑这条。

    判据:
      交集里 100% 一致            → 同一套源码 + 同一份配置(含 struct 布局), crc 可直用
      只有少数几个不一致          → 配置漂移, 不匹配的多是 module_layout 这类
                                    依赖 struct 布局的符号, 改 CONFIG 能对上
      大面积不一致(>1%)           → 源码版本不同(genksyms 结果随函数/类型签名变)
    """
    a, a_order = load_symvers(a_path)
    b, b_order = load_symvers(b_path)

    common = [n for n in a_order if n in b]
    same = [n for n in common if a[n] == b[n]]
    bad = [(n, a[n], b[n]) for n in common if a[n] != b[n]]
    only_a = [n for n in a_order if n not in b]
    only_b = [n for n in b_order if n not in a]

    print("A = %s  %d 个符号" % (a_path, len(a)))
    print("B = %s  %d 个符号" % (b_path, len(b)))
    print("交集 %d，一致 %d，不一致 %d" % (len(common), len(same), len(bad)))
    if bad:
        print("!! 不一致清单（前 40）:")
        for n, ca, cb in bad[:40]:
            print("   %-40s A 0x%s  B 0x%s" % (n, ca, cb))
        if len(bad) > 40:
            print("   ... 还有 %d 个" % (len(bad) - 40))
    if only_a:
        print("只在 A 里 %d 个（前 10）: %s" % (len(only_a), ", ".join(only_a[:10])))
    if only_b:
        print("只在 B 里 %d 个（前 10）: %s" % (len(only_b), ", ".join(only_b[:10])))

    if not common:
        print("结论: 两张表没有交集, 不是同一套内核")
        return 1
    if not bad:
        print("结论: crc 完全一致 —— 这套源码/config 编出来的 ko 可直接装, 不需要 merge")
        return 0
    ratio = 100.0 * len(bad) / len(common)
    if ratio <= 1.0:
        print("结论: 配置漂移（%.1f%% 不一致）。优先核对 struct module 相关的:" % ratio)
        for k in ('CONFIG_MODULE_UNLOAD', 'CONFIG_MODULE_SIG', 'CONFIG_KALLSYMS',
                  'CONFIG_KALLSYMS_ALL', 'CONFIG_LIVEPATCH', 'CONFIG_TRACEPOINTS',
                  'CONFIG_FTRACE', 'CONFIG_JUMP_LABEL', 'CONFIG_SMP', 'CONFIG_PREEMPT'):
            print("   - %s" % k)
        print("     改不动的就用 merge 用设备 crc 覆盖（本轮 merge 子命令）")
    else:
        print("结论: 源码版本不同（%.1f%% 不一致）。换 tag, 不要靠改配置硬凑。" % ratio)
    return 1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == 'dump':
        cmd_dump(sys.argv[2:])
    elif cmd == 'harvest':
        cmd_harvest(sys.argv[2], sys.argv[3:])
    elif cmd == 'merge':
        cmd_merge(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == 'check':
        return cmd_check(sys.argv[2], sys.argv[3])
    elif cmd == 'diff':
        return cmd_diff(sys.argv[2], sys.argv[3])
    elif cmd == 'count':
        return cmd_count(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
