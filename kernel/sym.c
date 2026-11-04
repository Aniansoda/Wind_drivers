#include "sym.h"
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/memblock.h>
#include <asm/memory.h>

u64 wind_memstart_addr = 0;
u64 wind_mem_end_addr = 0;

/* contpte_ptep_get: arm64 6.12 contpte 机制的 PTE 读取 (ptep_get 展开引用) */
typedef pte_t (*contpte_ptep_get_fn)(pte_t *);
static contpte_ptep_get_fn g_contpte_ptep_get = NULL;

/* __arch_clear_user: clear_user 展开引用, 返回未清零的字节数 */
typedef unsigned long (*arch_clear_user_fn)(void __user *, unsigned long);
static arch_clear_user_fn g_arch_clear_user = NULL;

/* set_memory_rw / set_memory_ro: arm64 页表权限切换 (fops rodata 写保护用) */
typedef int (*set_memory_fn)(unsigned long, int);
static set_memory_fn g_set_memory_rw = NULL;
static set_memory_fn g_set_memory_ro = NULL;

/*
 * 重写 phys_to_virt: arm64 linear map 下
 *   virt = phys - memstart_addr + PAGE_OFFSET
 * PAGE_OFFSET 是编译期宏 (asm/memory.h), memstart_addr 运行时解析。
 * 解析失败返回 NULL, 调用方走 ioremap 安全路径。
 */
void *wind_phys_to_virt(phys_addr_t pa)
{
	if (unlikely(!wind_memstart_addr))
		return NULL;
	return (void *)((unsigned long)pa - wind_memstart_addr + PAGE_OFFSET);
}

/*
 * 重写 pfn_valid: 判断物理页帧是否落在 DRAM 范围。
 * 解析失败时保守返回 false, 调用方走 ioremap 安全路径 (慢但正确)。
 */
bool wind_pfn_valid(unsigned long pfn)
{
	phys_addr_t pa;

	if (!wind_mem_end_addr)
		return false;
	pa = (phys_addr_t)pfn << PAGE_SHIFT;
	if (pa < wind_memstart_addr)
		return false;
	return pa < wind_mem_end_addr;
}

/*
 * 重写 ptep_get: 优先用 contpte_ptep_get (正确处理 PTE 保护位与 contpte),
 * 解析失败则回退 READ_ONCE 直读 (CONFIG_ARM64_CONTPE=n 时 ptep_get 即此语义)。
 */
pte_t wind_ptep_get(pte_t *ptep)
{
	if (g_contpte_ptep_get)
		return g_contpte_ptep_get(ptep);
	return READ_ONCE(*ptep);
}

/*
 * 重写 clear_user: 优先用 __arch_clear_user, 解析失败回退逐字节清零。
 */
unsigned long wind_clear_user(void __user *to, unsigned long n)
{
	if (g_arch_clear_user)
		return g_arch_clear_user(to, n);

	/* fallback: 内核态逐字节写 0, 等价 clear_user 语义 */
	unsigned long i;
	char __user *p = (char __user *)to;
	for (i = 0; i < n; i++) {
		if (put_user(0, p + i))
			return n - i;
	}
	return 0;
}

int wind_set_memory_rw(unsigned long addr, int numpages)
{
	if (!g_set_memory_rw)
		return -EOPNOTSUPP;
	return g_set_memory_rw(addr, numpages);
}

int wind_set_memory_ro(unsigned long addr, int numpages)
{
	if (!g_set_memory_ro)
		return -EOPNOTSUPP;
	return g_set_memory_ro(addr, numpages);
}

int wind_sym_init(void)
{
	u64 *p_memstart;

	/* memstart_addr: arm64 物理内存起始 */
	p_memstart = (u64 *)kallsyms_lookup_name("memstart_addr");
	if (p_memstart)
		wind_memstart_addr = *p_memstart;

	/* 物理内存结束: 用 totalram_pages (EXPORT_SYMBOL) 估算, 保守安全 */
	if (wind_memstart_addr)
		wind_mem_end_addr = wind_memstart_addr + (u64)totalram_pages * PAGE_SIZE;

	/* contpte_ptep_get */
	g_contpte_ptep_get = (contpte_ptep_get_fn)kallsyms_lookup_name("contpte_ptep_get");

	/* __arch_clear_user */
	g_arch_clear_user = (arch_clear_user_fn)kallsyms_lookup_name("__arch_clear_user");

	/* set_memory_rw / set_memory_ro */
	g_set_memory_rw = (set_memory_fn)kallsyms_lookup_name("set_memory_rw");
	g_set_memory_ro = (set_memory_fn)kallsyms_lookup_name("set_memory_ro");

	pr_info("wind: sym memstart=0x%llx memend=0x%llx contpte=%px clear_user=%px set_mem=%px/%px\n",
		wind_memstart_addr, wind_mem_end_addr,
		(void *)g_contpte_ptep_get, (void *)g_arch_clear_user,
		(void *)g_set_memory_rw, (void *)g_set_memory_ro);

	return 0;
}

void wind_sym_exit(void)
{
	wind_memstart_addr = 0;
	wind_mem_end_addr = 0;
	g_contpte_ptep_get = NULL;
	g_arch_clear_user = NULL;
	g_set_memory_rw = NULL;
	g_set_memory_ro = NULL;
}
