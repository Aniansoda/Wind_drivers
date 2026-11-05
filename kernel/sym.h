#ifndef _WIND_SYM_H_
#define _WIND_SYM_H_

/*
 * sym.h — GKI 白名单外符号运行时解析层
 *
 * GKI 冻结 KMI, vendor 模块只能引用白名单符号 (abi_symbollist 交集)。
 * 本驱动用到的若干内核函数不在官方 prebuilt .ko 的 __versions 段里
 * (modpost 报 undefined, 真机按冻结 CRC 加载时对不上), 统一改用
 * register_kprobe (EXPORT_SYMBOL_GPL, 白名单内) 按符号名取函数地址,
 * 编译期不产生对这些符号的 CRC 引用。
 *
 * 只适用于函数符号 (KALLSYMS_ALL=y 下所有非 static 函数都在 kallsyms 表)。
 * 数据符号 (memstart_addr / cache_line_size 等) 拿不到, 由 compat.h
 * 用白名单内联指令或硬编码替代, 不依赖数据符号。
 */

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/page.h>
#include <asm/pgtable.h>

struct mm_struct;
struct vm_area_struct;

/* 重写的 arch 内联逻辑 */
pte_t wind_ptep_get(pte_t *ptep);
unsigned long wind_clear_user(void __user *to, unsigned long n);

/* 重写的页表权限 (arm64 set_memory_* 非白名单) */
int wind_set_memory_rw(unsigned long addr, int numpages);
int wind_set_memory_ro(unsigned long addr, int numpages);

/* 运行时解析的内核函数 (白名单外, register_kprobe 取地址) */
void wind_mmput(struct mm_struct *mm);
void wind_iounmap(volatile void __iomem *addr);
int wind_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
			 unsigned long pfn, unsigned long size, pgprot_t prot);

int wind_sym_init(void);
void wind_sym_exit(void);

#endif /* _WIND_SYM_H_ */
