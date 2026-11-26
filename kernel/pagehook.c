// SPDX-License-Identifier: GPL-2.0
/*
 * pagehook.c — 页级执行补丁 hook (对齐 Satan 的 HOOK_MEMORY / SET_HOOK_VIEW)
 *
 * 同 VA 双物理页:
 *   原始页 = 用户可读, 不可执行 (UXN|PXN)
 *   影子页 = 已写入补丁字节, 可执行 (PX 放开)
 *
 * 执行流:
 *   CPU 取指命中原始页 -> IABT -> 切影子页 -> 重试取指 -> 跑补丁
 *   用户读取命中影子页 -> DABT -> 切原始页 -> 重试读取 -> 读到原字节
 *
 * 与 clonebp 的差别: 这里不写 BRK、不记录命中、不改写寄存器,
 * 只做「持续替换执行内容」。两者可同时存在, 各自按 page 命中分流。
 */
#include <linux/module.h>

/* 由 entry.c 定义: 卸载中标志 */
extern bool g_wind_unloading;
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/spinlock.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/pid.h>
#include <linux/pgtable.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>

#include "pagehook.h"
#include "comm.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))
#define ph_kmap(pg)      kmap_atomic(pg)
#define ph_kunmap(pg)    kunmap_atomic(pg)
#else
#define ph_kmap(pg)      kmap_local_page(pg)
#define ph_kunmap(pg)    kunmap_local(pg)
#endif

#define PH_MAX_PAGES       PH_MAX_POINTS
#define PH_STATE_ORIG      0
#define PH_STATE_SHADOW    1
#define PH_STATE_BAILOUT   2
#define PH_BAILOUT_MAX     16
#define PH_BAILOUT_WINDOW  (HZ / 4)

struct ph_page {
	struct mm_struct *mm;
	u64     va_page;          /* 4KB 对齐的用户 VA */
	struct page *orig_page;
	struct page *shadow_page;
	int     inited;
	atomic_t state;           /* PH_STATE_* */
	atomic_t bailout_cnt;
	unsigned long bailout_last;
	u32     patch_len;        /* 日志用 */
	atomic64_t ifault;
	atomic64_t dfault;
};

static DEFINE_MUTEX(g_ph_lock);
static struct ph_page g_ph_pages[PH_MAX_PAGES];
static int g_ph_page_cnt;
static struct mm_struct *g_ph_mm;

static atomic64_t g_ph_ifault;
static atomic64_t g_ph_dfault;
static atomic64_t g_ph_switch;
static atomic64_t g_ph_view_orig;

/* ------------------------------------------------------- ESR / PTE */
static inline int ph_esr_is_iabt(unsigned int esr)
{
	return ((esr >> 26) & 0x3F) == 0x21 || ((esr >> 26) & 0x3F) == 0x20;
}

static inline int ph_esr_is_dabt(unsigned int esr)
{
	unsigned int ec = (esr >> 26) & 0x3F;

	return ec == 0x24 || ec == 0x25;
}

/* 取目标地址 pte 指针(调用方须持 mmap_read_lock) */
static pte_t *ph_pte_get(struct mm_struct *mm, unsigned long addr)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return NULL;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d))
		return NULL;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud))
		return NULL;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return NULL;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0))
	/* 6.5+ pte_offset_map 展开成 __pte_offset_map, 不导出给模块,
	 * 等价手算: 页基址走 pmd_page_vaddr, 再叠页内下标 */
	return (pte_t *)pmd_page_vaddr(*pmd) + pte_index(addr);
#else
	return pte_offset_kernel(pmd, addr);
#endif
}

static void ph_flush_va_local(struct mm_struct *mm, unsigned long va)
{
	int cpu;

	if (!mm)
		return;
	preempt_disable();
	cpu = smp_processor_id();
	if (mm_cpumask(mm) && cpumask_test_cpu(cpu, mm_cpumask(mm)))
		local_flush_tlb_page(va);
	preempt_enable();
}

static struct ph_page *ph_find(u64 page_base)
{
	int i;

	for (i = 0; i < g_ph_page_cnt; ++i) {
		if (g_ph_pages[i].inited && g_ph_pages[i].va_page == page_base)
			return &g_ph_pages[i];
	}
	return NULL;
}

/* ------------------------------------------------- 页状态切换 */
/* 不可在这里 sleep: 会被 IABT/DABT 异常路径调用 */
static int ph_switch_raw(struct mm_struct *mm, struct ph_page *pp, int to_shadow)
{
	pte_t *ptep;
	pte_t pte, cur;
	unsigned long flags;

	if (!mm || !pp || !pp->inited)
		return -EINVAL;
	if (atomic_read(&pp->state) == to_shadow)
		return 0;
	if (!pp->orig_page || PageCompound(pp->orig_page))
		return -EOPNOTSUPP;

	ptep = ph_pte_get(mm, pp->va_page);
	if (!ptep)
		return -EFAULT;

	cur = READ_ONCE(*ptep);
	if (pte_cont(cur))   /* 折叠 PTE 会连带改相邻页, 拒绝 */
		return -EOPNOTSUPP;

	local_irq_save(flags);
	if (to_shadow == PH_STATE_SHADOW) {
		/* 影子页: 用户不可读(无 PTE_USER), 可执行(PX 放开) */
		pte = mk_pte(pp->shadow_page, __pgprot(PTE_PXN));
	} else if (to_shadow == PH_STATE_BAILOUT) {
		/* 兜底: 原始页放宽为可读可执行, 打破异常环 */
		pte = mk_pte(pp->orig_page,
			     __pgprot(PTE_USER | PTE_RDONLY | PTE_PXN));
	} else {
		/* 原始页: 用户可读, 不可执行 */
		pte = mk_pte(pp->orig_page,
			     __pgprot(PTE_USER | PTE_RDONLY | PTE_UXN | PTE_PXN));
	}
	WRITE_ONCE(*ptep, pte);
	local_irq_restore(flags);

	atomic_set(&pp->state, to_shadow);
	ph_flush_va_local(mm, pp->va_page);
	atomic64_inc(&g_ph_switch);
	return 0;
}

/* 进程上下文调用: 自己取 mmap_lock */
static int ph_switch(struct mm_struct *mm, struct ph_page *pp, int to_shadow)
{
	int ret;

	if (!mm || !pp || !pp->inited)
		return -EINVAL;
	if (atomic_read(&pp->state) == to_shadow)
		return 0;

	mmap_read_lock(mm);
	ret = ph_switch_raw(mm, pp, to_shadow);
	mmap_read_unlock(mm);
	return ret;
}

/* 异常环保护: 同一页短时间反复异常就退到兜底态, 放行原始行为 */
static int ph_try_bailout(struct ph_page *pp)
{
	unsigned long now = jiffies;

	if (time_after(now, pp->bailout_last + PH_BAILOUT_WINDOW))
		atomic_set(&pp->bailout_cnt, 0);
	pp->bailout_last = now;

	if (atomic_inc_return(&pp->bailout_cnt) <= PH_BAILOUT_MAX)
		return 0;

	if (atomic_read(&pp->state) != PH_STATE_BAILOUT) {
		ph_switch_raw(pp->mm, pp, PH_STATE_BAILOUT);
		if (printk_ratelimit())
			pr_warn("[pagehook] 页 %llx 异常环, 退到兜底态\n",
				(unsigned long long)pp->va_page);
	}
	return 1;
}

/* ------------------------------------------------- 异常接管 */
static int ph_mem_abort_do(unsigned long far, unsigned int esr)
{
	struct ph_page *pp = ph_find((u64)(far & PAGE_MASK));

	if (!pp)
		return 0;

	if (ph_esr_is_iabt(esr)) {
		/* 取指命中原始页(UXN) -> 换影子页 -> 重试取指 */
		if (atomic_read(&pp->state) == PH_STATE_BAILOUT)
			return 0;
		if (ph_try_bailout(pp))
			return 1;
		ph_switch_raw(g_ph_mm, pp, PH_STATE_SHADOW);
		atomic64_inc(&pp->ifault);
		atomic64_inc(&g_ph_ifault);
		return 1;
	}

	if (ph_esr_is_dabt(esr)) {
		/* 用户读取命中影子页(无 USER 位) -> 换回原始页 -> 重试 */
		if (atomic_read(&pp->state) != PH_STATE_SHADOW)
			return 0;
		if (ph_try_bailout(pp))
			return 1;
		ph_switch_raw(g_ph_mm, pp, PH_STATE_ORIG);
		atomic64_inc(&pp->dfault);
		atomic64_inc(&g_ph_dfault);
		atomic64_inc(&g_ph_view_orig);
		return 1;
	}

	return 0;
}

/* el0t_64_sync_handler pre_handler: EL0 同步异常总入口。
 * x0 = kernel_entry 保存的用户现场 pt_regs。
 * far/esr 必须 read_sysreg, 不能从 regs->regs[] 取(那是用户 x0/x1)。 */
static int ph_el0_sync_pre(struct kprobe *kpr, struct pt_regs *regs)
{
	unsigned long esr, far;
	struct pt_regs *uregs;
	int ret;

	if (READ_ONCE(g_wind_unloading))
		return 0;

	(void)kpr;
	if (!g_ph_mm || g_ph_page_cnt == 0)
		return 0;

	uregs = (struct pt_regs *)regs->regs[0];
	if (!uregs || !user_mode(uregs))
		return 0;

	esr = read_sysreg(esr_el1);
	if (!ph_esr_is_iabt(esr) && !ph_esr_is_dabt(esr))
		return 0;

	far = read_sysreg(far_el1);
	ret = ph_mem_abort_do(far, (unsigned int)esr);
	if (ret) {
		/* 直接让 el0t_64_sync_handler 返回: 异常返回后 CPU 用新页表
		 * 重试那条指令。不改 pc 会拿同一组 far/esr 再走一遍 ->
		 * 切页/报错无限循环 -> 卡死重启。 */
		regs->pc = regs->regs[30];
	}
	return ret;
}

/* mprotect 会按 VMA 重建 PTE。此刻若挂着影子页, 新 PTE 会保留影子 PFN
 * 并改成用户可读 -> 补丁字节被直接读出来。重建前切回原始页。 */
static int ph_mprotect_pre(struct kprobe *kpr, struct pt_regs *regs)
{
	unsigned long start, len, va;
	struct ph_page *pp;

	if (READ_ONCE(g_wind_unloading))
		return 0;

	(void)kpr;
	if (!g_ph_mm || g_ph_page_cnt == 0)
		return 0;

	start = regs->regs[0];
	len   = regs->regs[1];
	if (!len)
		return 0;

	for (va = start & PAGE_MASK; va < start + len; va += PAGE_SIZE) {
		pp = ph_find((u64)va);
		if (!pp || !pp->inited)
			continue;
		if (atomic_read(&pp->state) == PH_STATE_SHADOW)
			ph_switch_raw(g_ph_mm, pp, PH_STATE_ORIG);
	}
	return 0;
}

/* 进程退出: 所有页恢复原始页, 防影子 PFN 随 mm 释放后残留 */
static int ph_mm_release_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct mm_struct *mm = (struct mm_struct *)regs->regs[0];
	int i;

	(void)p;
	if (!mm || mm != g_ph_mm)
		return 0;

	for (i = 0; i < g_ph_page_cnt; ++i) {
		struct ph_page *pp = &g_ph_pages[i];

		if (!pp->inited)
			continue;
		if (atomic_read(&pp->state) == PH_STATE_SHADOW)
			ph_switch_raw(mm, pp, PH_STATE_ORIG);
	}
	return 0;
}

/* ------------------------------------------------- kprobe 注册 */
static struct kprobe ph_el0_sync_kp = {
	.symbol_name = "el0t_64_sync_handler",
};
static struct kprobe ph_mprotect_kp = {
	.symbol_name = "do_mprotect_pkey",
};
static struct kprobe ph_mm_release_kp = {
	.symbol_name = "mmput",   /* mmput->__mmput 前拦, 见下方 set 里的说明 */
};

static int ph_kp_el0_reg;
static int ph_kp_mprot_reg;
static int ph_kp_mmrel_reg;

static int ph_install_hooks(void)
{
	int ret;

	ph_el0_sync_kp.pre_handler = ph_el0_sync_pre;
	ret = register_kprobe(&ph_el0_sync_kp);
	if (ret) {
		pr_warn("[pagehook] el0t_64_sync_handler probe failed: %d\n", ret);
		return ret;
	}
	ph_kp_el0_reg = 1;

	ph_mprotect_kp.pre_handler = ph_mprotect_pre;
	ret = register_kprobe(&ph_mprotect_kp);
	if (ret == 0)
		ph_kp_mprot_reg = 1;
	else
		pr_warn("[pagehook] do_mprotect_pkey probe failed: %d\n", ret);

	ph_mm_release_kp.pre_handler = ph_mm_release_pre;
	ret = register_kprobe(&ph_mm_release_kp);
	if (ret == 0)
		ph_kp_mmrel_reg = 1;
	else
		pr_warn("[pagehook] mmput probe failed: %d\n", ret);

	return 0;
}

static void ph_remove_hooks(void)
{
	if (ph_kp_mmrel_reg) {
		unregister_kprobe(&ph_mm_release_kp);
		ph_kp_mmrel_reg = 0;
	}
	if (ph_kp_mprot_reg) {
		unregister_kprobe(&ph_mprotect_kp);
		ph_kp_mprot_reg = 0;
	}
	if (ph_kp_el0_reg) {
		unregister_kprobe(&ph_el0_sync_kp);
		ph_kp_el0_reg = 0;
	}
}

/* ------------------------------------------------- 对外接口 */
/* 安装: 克隆目标页并写入 patch, 页先停原始态(不可执行),
 * 首次取指由 IABT 自动切影子页。 */
int pagehook_install(struct mm_struct *mm, u64 addr, const u8 *patch, u32 patch_len)
{
	struct ph_page *pp = NULL;
	struct page *opage;
	pte_t *ptep;
	pte_t pte;
	void *dst, *src;
	u64 page = addr & PAGE_MASK;
	u32 off = addr & ~PAGE_MASK;
	int i, ret = 0;
	struct page *spage;

	if (!mm || !patch || !patch_len || patch_len > PH_PATCH_MAX)
		return -EINVAL;
	if (off + patch_len > PAGE_SIZE)
		return -EINVAL;

	if (!ph_kp_el0_reg && g_ph_page_cnt == 0) {
		ret = ph_install_hooks();
		if (ret)
			return ret;
	}

	mutex_lock(&g_ph_lock);

	/* 已有同页 hook: 先拆, 支持热更新 */
	for (i = 0; i < g_ph_page_cnt; ++i) {
		if (g_ph_pages[i].inited && g_ph_pages[i].va_page == page &&
		    g_ph_pages[i].mm == mm) {
			mutex_unlock(&g_ph_lock);
			pagehook_remove(mm, addr);
			mutex_lock(&g_ph_lock);
			break;
		}
	}

	for (i = 0; i < PH_MAX_PAGES; ++i) {
		if (!g_ph_pages[i].inited) {
			pp = &g_ph_pages[i];
			break;
		}
	}
	if (!pp) {
		ret = -ENOSPC;
		goto out;
	}

	/* 取原始页并克隆 */
	mmap_read_lock(mm);
	ptep = ph_pte_get(mm, page);
	if (!ptep || !pte_present(*ptep)) {
		mmap_read_unlock(mm);
		ret = -EFAULT;
		goto out;
	}
	pte = *ptep;
	opage = pfn_to_page(pte_pfn(pte));
	if (!opage || PageCompound(opage)) {
		mmap_read_unlock(mm);
		ret = -EOPNOTSUPP;
		goto out;
	}
	get_page(opage);
	spage = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!spage) {
		mmap_read_unlock(mm);
		put_page(opage);
		ret = -ENOMEM;
		goto out;
	}
	dst = ph_kmap(spage);
	src = ph_kmap(opage);
	memcpy(dst, src, PAGE_SIZE);
	memcpy((u8 *)dst + off, patch, patch_len);
	ph_kunmap(src);
	ph_kunmap(dst);
	mmap_read_unlock(mm);

	pp->mm = mm;
	pp->va_page = page;
	pp->orig_page = opage;
	pp->shadow_page = spage;
	pp->patch_len = patch_len;
	atomic_set(&pp->state, PH_STATE_ORIG);
	atomic_set(&pp->bailout_cnt, 0);
	pp->bailout_last = 0;
	atomic64_set(&pp->ifault, 0);
	atomic64_set(&pp->dfault, 0);
	pp->inited = 1;

	if (!g_ph_mm)
		g_ph_mm = mm;
	if (i >= g_ph_page_cnt)
		g_ph_page_cnt = i + 1;

	/* 原始页设为不可执行, 首次取指触发 IABT */
	ph_switch(mm, pp, PH_STATE_ORIG);

	pr_info("[pagehook] install va=%llx len=%u\n",
		(unsigned long long)addr, patch_len);
out:
	mutex_unlock(&g_ph_lock);
	return ret;
}

int pagehook_remove(struct mm_struct *mm, u64 addr)
{
	struct ph_page *pp;
	u64 page = addr & PAGE_MASK;
	int i;

	if (!mm)
		return -EINVAL;

	mutex_lock(&g_ph_lock);
	pp = ph_find(page);
	if (!pp || pp->mm != mm) {
		mutex_unlock(&g_ph_lock);
		return -ENOENT;
	}

	/* 先切回原始页, 再释放影子页, 顺序不能反 */
	ph_switch(mm, pp, PH_STATE_ORIG);
	if (pp->shadow_page)
		put_page(pp->shadow_page);
	if (pp->orig_page)
		put_page(pp->orig_page);

	memset(pp, 0, sizeof(*pp));

	/* 收缩计数, 全空则摘 kprobe */
	for (i = g_ph_page_cnt - 1; i >= 0; --i) {
		if (g_ph_pages[i].inited)
			break;
	}
	g_ph_page_cnt = i + 1;
	if (g_ph_page_cnt <= 0) {
		g_ph_page_cnt = 0;
		g_ph_mm = NULL;
	}
	mutex_unlock(&g_ph_lock);

	if (g_ph_page_cnt == 0)
		ph_remove_hooks();
	return 0;
}

/* SET_HOOK_VIEW(DATA): 强制切回原始页, 供外部读取补丁前的字节 */
int pagehook_set_view(struct mm_struct *mm, u64 addr, u32 view)
{
	struct ph_page *pp;
	u64 page = addr & PAGE_MASK;
	int ret;

	if (!mm)
		return -EINVAL;
	if (view != PH_VIEW_DATA)
		return -EINVAL;   /* 只提供 DATA 视图, 不提供 EXECUTE */

	mutex_lock(&g_ph_lock);
	pp = ph_find(page);
	if (!pp || pp->mm != mm) {
		mutex_unlock(&g_ph_lock);
		return -ENOENT;
	}
	ret = ph_switch(mm, pp, PH_STATE_ORIG);
	if (ret == 0)
		atomic64_inc(&g_ph_view_orig);
	mutex_unlock(&g_ph_lock);
	return ret;
}

void pagehook_get_stat(PAGEHOOK_STAT *out)
{
	int i;

	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->page_cnt = g_ph_page_cnt;
	out->ifault = (u64)atomic64_read(&g_ph_ifault);
	out->dfault = (u64)atomic64_read(&g_ph_dfault);
	out->switch_cnt = (u64)atomic64_read(&g_ph_switch);
	out->view_orig = (u64)atomic64_read(&g_ph_view_orig);
	for (i = 0; i < g_ph_page_cnt; ++i) {
		if (!g_ph_pages[i].inited)
			continue;
		out->pages[out->page_cnt_used].va = g_ph_pages[i].va_page;
		out->pages[out->page_cnt_used].state = atomic_read(&g_ph_pages[i].state);
		out->pages[out->page_cnt_used].ifault =
			(u64)atomic64_read(&g_ph_pages[i].ifault);
		out->pages[out->page_cnt_used].dfault =
			(u64)atomic64_read(&g_ph_pages[i].dfault);
		if (++out->page_cnt_used >= PAGEHOOK_STAT_MAX_PAGES)
			break;
	}
}

/* 模块退出: 全部还原再摘 kprobe */
void pagehook_module_exit(void)
{
	int i;

	for (i = 0; i < g_ph_page_cnt; ++i) {
		struct ph_page *pp = &g_ph_pages[i];

		if (!pp->inited)
			continue;
		ph_switch(pp->mm, pp, PH_STATE_ORIG);
		if (pp->shadow_page)
			put_page(pp->shadow_page);
		if (pp->orig_page)
			put_page(pp->orig_page);
		memset(pp, 0, sizeof(*pp));
	}
	g_ph_page_cnt = 0;
	g_ph_mm = NULL;
	ph_remove_hooks();
}

int pagehook_module_init(void)
{
	memset(g_ph_pages, 0, sizeof(g_ph_pages));
	g_ph_page_cnt = 0;
	g_ph_mm = NULL;
	return 0;
}
