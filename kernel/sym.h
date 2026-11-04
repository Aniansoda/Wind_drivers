#ifndef _WIND_SYM_H_
#define _WIND_SYM_H_

/*
 * sym.h — GKI 非白名单符号运行时解析层
 *
 * GKI 冻结 KMI, vendor 模块只能引用白名单符号。驱动用到的 arch 内部符号
 * (phys_to_virt / pfn_valid / ptep_get / clear_user 展开引用的 memstart_addr /
 * mem_section / contpte_ptep_get / __arch_clear_user 等) 不在白名单, 编译期
 * 直接引用会报 modpost undefined。这里用 kallsyms_lookup_name 运行时解析,
 * 并重写对应的内联逻辑, 绕开编译期 CRC 检查。
 */

#include <linux/types.h>
#include <asm/page.h>
#include <asm/pgtable.h>

/* 运行时解析出的物理内存布局 (由 wind_sym_init 填充) */
extern u64 wind_memstart_addr;
extern u64 wind_mem_end_addr;

/* 重写的 arch 内联逻辑 */
void *wind_phys_to_virt(phys_addr_t pa);
bool wind_pfn_valid(unsigned long pfn);
pte_t wind_ptep_get(pte_t *ptep);

/* 重写的页表权限 (arm64 set_memory_* 非 EXPORT_SYMBOL) */
int wind_set_memory_rw(unsigned long addr, int numpages);
int wind_set_memory_ro(unsigned long addr, int numpages);

int wind_sym_init(void);
void wind_sym_exit(void);

#endif /* _WIND_SYM_H_ */
