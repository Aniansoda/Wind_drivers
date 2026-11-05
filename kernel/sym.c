#include "sym.h"
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/io.h>

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

/* mmput: 释放 mm 引用 (EXPORT_SYMBOL_GPL, 但官方 .ko 不引用, 运行时解析) */
typedef void (*mmput_fn)(struct mm_struct *);
static mmput_fn g_mmput = NULL;

/* iounmap: 解除 ioremap 映射 (白名单外, 运行时解析) */
typedef void (*iounmap_fn)(volatile void __iomem *);
static iounmap_fn g_iounmap = NULL;

/* remap_pfn_range: mmap 目标物理页 (白名单外, 运行时解析) */
typedef int (*remap_pfn_range_fn)(struct vm_area_struct *, unsigned long,
				  unsigned long, unsigned long, pgprot_t);
static remap_pfn_range_fn g_remap_pfn_range = NULL;

/*
 * 用 register_kprobe 按符号名解析函数地址。
 * kallsyms_lookup_name 的 export 被 GKI 移除, 不能直接调用;
 * register_kprobe 是白名单符号, 内部走内核 kallsyms 解析符号名
 * (KALLSYMS_ALL=y 下所有非 static 函数都在表里)。
 * 只适用于函数符号 (代码段), 数据符号拿不到。
 */
static unsigned long wind_kprobe_resolve(const char *name)
{
	struct kprobe kp;
	unsigned long addr;
	int ret;

	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = name;
	ret = register_kprobe(&kp);
	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
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
	unsigned long i;
	char __user *p;

	if (g_arch_clear_user)
		return g_arch_clear_user(to, n);

	/* fallback: 内核态逐字节写 0, 等价 clear_user 语义 */
	p = (char __user *)to;
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

void wind_mmput(struct mm_struct *mm)
{
	if (g_mmput)
		g_mmput(mm);
}

void wind_iounmap(volatile void __iomem *addr)
{
	if (g_iounmap)
		g_iounmap(addr);
}

int wind_remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
			 unsigned long pfn, unsigned long size, pgprot_t prot)
{
	if (!g_remap_pfn_range)
		return -EOPNOTSUPP;
	return g_remap_pfn_range(vma, addr, pfn, size, prot);
}

int wind_sym_init(void)
{
	g_contpte_ptep_get = (contpte_ptep_get_fn)wind_kprobe_resolve("contpte_ptep_get");
	g_arch_clear_user = (arch_clear_user_fn)wind_kprobe_resolve("__arch_clear_user");
	g_set_memory_rw = (set_memory_fn)wind_kprobe_resolve("set_memory_rw");
	g_set_memory_ro = (set_memory_fn)wind_kprobe_resolve("set_memory_ro");
	g_mmput = (mmput_fn)wind_kprobe_resolve("mmput");
	g_iounmap = (iounmap_fn)wind_kprobe_resolve("iounmap");
	g_remap_pfn_range = (remap_pfn_range_fn)wind_kprobe_resolve("remap_pfn_range");

	pr_info("wind: sym contpte=%px clear_user=%px set_mem=%px/%px mmput=%px iounmap=%px remap=%px\n",
		(void *)g_contpte_ptep_get, (void *)g_arch_clear_user,
		(void *)g_set_memory_rw, (void *)g_set_memory_ro,
		(void *)g_mmput, (void *)g_iounmap, (void *)g_remap_pfn_range);

	return 0;
}

void wind_sym_exit(void)
{
	g_contpte_ptep_get = NULL;
	g_arch_clear_user = NULL;
	g_set_memory_rw = NULL;
	g_set_memory_ro = NULL;
	g_mmput = NULL;
	g_iounmap = NULL;
	g_remap_pfn_range = NULL;
}
