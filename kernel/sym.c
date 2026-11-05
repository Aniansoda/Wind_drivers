#include "sym.h"
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

/* contpte_ptep_get: arm64 contpte 机制的 PTE 读取 (ptep_get 展开引用), 2 参数 (ptep, orig_pte) */
typedef pte_t (*contpte_ptep_get_fn)(pte_t *, pte_t);
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

/* vmap 三件套: 物理地址 → 内核虚拟地址临时映射 (白名单外) */
typedef void (*free_vm_area_fn)(struct vm_struct *);
typedef struct vm_struct *(*get_vm_area_caller_fn)(unsigned long, unsigned long,
						   unsigned long, unsigned long,
						   const void *);
typedef int (*ioremap_page_range_fn)(unsigned long, unsigned long,
				     phys_addr_t, pgprot_t);
static free_vm_area_fn g_free_vm_area = NULL;
static get_vm_area_caller_fn g_get_vm_area_caller = NULL;
static ioremap_page_range_fn g_ioremap_page_range = NULL;

/* 文件操作 (白名单外) */
typedef struct file *(*filp_open_fn)(const char *, int, umode_t);
typedef ssize_t (*kernel_read_fn)(struct file *, void *, size_t, loff_t *);
typedef char *(*file_path_fn)(struct file *, char *, int);
static filp_open_fn g_filp_open = NULL;
static kernel_read_fn g_kernel_read = NULL;
static file_path_fn g_file_path = NULL;

/* 硬件断点 (白名单外) */
typedef struct perf_event *(*register_user_hw_breakpoint_fn)(struct perf_event_attr *,
							     perf_overflow_handler_t,
							     void *, struct task_struct *);
typedef void (*unregister_hw_breakpoint_fn)(struct perf_event *);
static register_user_hw_breakpoint_fn g_register_user_hw_breakpoint = NULL;
static unregister_hw_breakpoint_fn g_unregister_hw_breakpoint = NULL;

/* nofault 拷贝 (白名单外) */
typedef long (*copy_from_user_nofault_fn)(void *, const void __user *, size_t);
typedef long (*copy_to_user_nofault_fn)(void __user *, const void *, size_t);
static copy_from_user_nofault_fn g_copy_from_user_nofault = NULL;
static copy_to_user_nofault_fn g_copy_to_user_nofault = NULL;

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

/* 按符号名注册 kprobe hook, 供影子页/BRK 断点捕获 fault/异常。
 * 符号受 NOKPROBE 保护时 register_kprobe 失败, 返回 NULL。 */
struct kprobe *wind_kprobe_register_symbol(const char *name,
					  int (*pre)(struct kprobe *, struct pt_regs *),
					  void (*post)(struct kprobe *, struct pt_regs *, unsigned long))
{
	struct kprobe *kp;

	kp = kzalloc(sizeof(*kp), GFP_KERNEL);
	if (!kp)
		return NULL;
	kp->symbol_name = name;
	kp->pre_handler = pre;
	kp->post_handler = post;
	if (register_kprobe(kp) < 0) {
		kfree(kp);
		return NULL;
	}
	return kp;
}

void wind_kprobe_unregister_symbol(struct kprobe *kp)
{
	if (!kp)
		return;
	unregister_kprobe(kp);
	kfree(kp);
}

/*
 * 重写 ptep_get: 模拟内核 ptep_get 逻辑 —— 先 READ_ONCE 读原始 PTE,
 * 若为 contpte (VALID|CONT 位) 且 contpte_ptep_get 已解析, 走 2 参数
 * contpte_ptep_get(ptep, orig_pte); 否则直接返回原始 PTE。
 * CONFIG_ARM64_CONTPE=n 时 ptep_get 即 __ptep_get 语义 (纯 READ_ONCE)。
 */
static inline bool wind_pte_valid_cont(pte_t pte)
{
	return (pte_val(pte) & PTE_VALID) && (pte_val(pte) & PTE_CONT);
}

pte_t wind_ptep_get(pte_t *ptep)
{
	pte_t pte = READ_ONCE(*ptep);

	if (g_contpte_ptep_get && wind_pte_valid_cont(pte))
		return g_contpte_ptep_get(ptep, pte);
	return pte;
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

void wind_free_vm_area(struct vm_struct *area)
{
	if (g_free_vm_area)
		g_free_vm_area(area);
}

struct vm_struct *wind_get_vm_area_caller(unsigned long size, unsigned long flags,
					  unsigned long start, unsigned long end,
					  const void *caller)
{
	if (!g_get_vm_area_caller)
		return NULL;
	return g_get_vm_area_caller(size, flags, start, end, caller);
}

int wind_ioremap_page_range(unsigned long addr, unsigned long end,
			    phys_addr_t phys_addr, pgprot_t prot)
{
	if (!g_ioremap_page_range)
		return -EOPNOTSUPP;
	return g_ioremap_page_range(addr, end, phys_addr, prot);
}

struct file *wind_filp_open(const char *filename, int flags, umode_t mode)
{
	if (!g_filp_open)
		return ERR_PTR(-EOPNOTSUPP);
	return g_filp_open(filename, flags, mode);
}

ssize_t wind_kernel_read(struct file *file, void *buf, size_t count, loff_t *pos)
{
	if (!g_kernel_read)
		return -EOPNOTSUPP;
	return g_kernel_read(file, buf, count, pos);
}

char *wind_file_path(struct file *filp, char *buf, int buflen)
{
	if (!g_file_path)
		return ERR_PTR(-EOPNOTSUPP);
	return g_file_path(filp, buf, buflen);
}

struct perf_event *wind_register_user_hw_breakpoint(struct perf_event_attr *attr,
						    wind_perf_overflow_handler_t triggered,
						    void *context, struct task_struct *tsk)
{
	if (!g_register_user_hw_breakpoint)
		return ERR_PTR(-EOPNOTSUPP);
	return g_register_user_hw_breakpoint(attr, (perf_overflow_handler_t)triggered,
					     context, tsk);
}

void wind_unregister_hw_breakpoint(struct perf_event *bp)
{
	if (g_unregister_hw_breakpoint)
		g_unregister_hw_breakpoint(bp);
}

long wind_copy_from_user_nofault(void *dst, const void __user *src, size_t size)
{
	if (!g_copy_from_user_nofault)
		return -EFAULT;
	return g_copy_from_user_nofault(dst, src, size);
}

long wind_copy_to_user_nofault(void __user *dst, const void *src, size_t size)
{
	if (!g_copy_to_user_nofault)
		return -EFAULT;
	return g_copy_to_user_nofault(dst, src, size);
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
	g_free_vm_area = (free_vm_area_fn)wind_kprobe_resolve("free_vm_area");
	g_get_vm_area_caller = (get_vm_area_caller_fn)wind_kprobe_resolve("__get_vm_area_caller");
	g_ioremap_page_range = (ioremap_page_range_fn)wind_kprobe_resolve("ioremap_page_range");
	g_filp_open = (filp_open_fn)wind_kprobe_resolve("filp_open");
	g_kernel_read = (kernel_read_fn)wind_kprobe_resolve("kernel_read");
	g_file_path = (file_path_fn)wind_kprobe_resolve("file_path");
	g_register_user_hw_breakpoint = (register_user_hw_breakpoint_fn)wind_kprobe_resolve("register_user_hw_breakpoint");
	g_unregister_hw_breakpoint = (unregister_hw_breakpoint_fn)wind_kprobe_resolve("unregister_hw_breakpoint");
	g_copy_from_user_nofault = (copy_from_user_nofault_fn)wind_kprobe_resolve("copy_from_user_nofault");
	g_copy_to_user_nofault = (copy_to_user_nofault_fn)wind_kprobe_resolve("copy_to_user_nofault");

	pr_info("wind: sym vmap=%px/%px/%px filp=%px kread=%px fpath=%px hwbp=%px/%px nofault=%px/%px\n",
		(void *)g_free_vm_area, (void *)g_get_vm_area_caller, (void *)g_ioremap_page_range,
		(void *)g_filp_open, (void *)g_kernel_read, (void *)g_file_path,
		(void *)g_register_user_hw_breakpoint, (void *)g_unregister_hw_breakpoint,
		(void *)g_copy_from_user_nofault, (void *)g_copy_to_user_nofault);

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
	g_free_vm_area = NULL;
	g_get_vm_area_caller = NULL;
	g_ioremap_page_range = NULL;
	g_filp_open = NULL;
	g_kernel_read = NULL;
	g_file_path = NULL;
	g_register_user_hw_breakpoint = NULL;
	g_unregister_hw_breakpoint = NULL;
	g_copy_from_user_nofault = NULL;
	g_copy_to_user_nofault = NULL;
}
