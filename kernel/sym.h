#ifndef _WIND_SYM_H_
#define _WIND_SYM_H_

/*
 * sym.h — GKI 非白名单符号运行时解析层
 *
 * GKI 冻结 KMI, vendor 模块只能引用白名单符号。驱动用到的 arch 内部函数
 * (contpte_ptep_get / __arch_clear_user / set_memory_rw / set_memory_ro)
 * 不在白名单, 且 kallsyms_lookup_name 的 export 被 GKI 移除, 不能用它
 * 解析。这里改用 register_kprobe (EXPORT_SYMBOL_GPL, 白名单内) 按符号名
 * 取函数地址, 绕开编译期 CRC 检查。
 *
 * phys_to_virt / pfn_valid 依赖的 memstart_addr 是数据符号, kprobe 拿不到,
 * 故不再重写, 改用 ioremap_cache (memory.c 进程上下文) 与 copy_from_user
 * (gyro.c 目标进程上下文) 替代, 彻底不依赖 memstart_addr。
 */

#include <linux/types.h>
#include <linux/compiler.h>
#include <asm/page.h>
#include <asm/pgtable.h>

/* 重写的 arch 内联逻辑 */
pte_t wind_ptep_get(pte_t *ptep);
unsigned long wind_clear_user(void __user *to, unsigned long n);

/* 重写的页表权限 (arm64 set_memory_* 非 EXPORT_SYMBOL) */
int wind_set_memory_rw(unsigned long addr, int numpages);
int wind_set_memory_ro(unsigned long addr, int numpages);

int wind_sym_init(void);
void wind_sym_exit(void);

#endif /* _WIND_SYM_H_ */
